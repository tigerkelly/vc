
#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <zip.h>

#include "vc.h"

// -------------------------------------------------------------------
// CommitInfo  –  metadata extracted from a commit zip's MANIFEST.txt
// -------------------------------------------------------------------
typedef struct {
    char archive[256];    // zip filename
    char date[32];       // date string from manifest
    char author[128];    // author
    char project[128];   // project name
    char branch[64];     // branch (not in manifest — derived from tags)
    char message[256];   // commit message
    int  fileCount;      // number of files in the commit
    long sizeBytes;      // size of the zip file on disk
    bool hasTag;         // whether a tag points to this commit
    char tagName[256];   // tag name(s) — comma-separated if multiple
} CommitInfo;

// -------------------------------------------------------------------
// readManifestField  –  extract a field value from MANIFEST.txt text.
// Looks for "field : value" format.
// -------------------------------------------------------------------
static bool readManifestField(const char *manifest, const char *field,
                               char *buf, size_t sz) {
    const char *p = manifest;
    size_t flen = strlen(field);

    while (*p) {
        if (strncmp(p, field, flen) == 0) {
            const char *v = p + flen;
            while (*v == ' ' || *v == ':' || *v == '\t') v++;
            const char *end = v;
            while (*end && *end != '\n') end++;
            size_t vlen = (size_t)(end - v);
            if (vlen >= sz) vlen = sz - 1;
            snprintf(buf, sz, "%.*s", (int)vlen, v);
            return true;
        }
        // Advance to next line.
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
}

// -------------------------------------------------------------------
// readManifestInt  –  extract an integer field from MANIFEST.txt.
// -------------------------------------------------------------------
static int readManifestInt(const char *manifest, const char *field) {
    char buf[32];
    if (readManifestField(manifest, field, buf, sizeof(buf)))
        return atoi(buf);
    return 0;
}

// -------------------------------------------------------------------
// loadManifestFromZip  –  open a zip and read MANIFEST.txt into a
// heap-allocated string.  Caller must free.  Returns NULL on failure.
// -------------------------------------------------------------------
static char *loadManifestFromZip(const char *zipPath) {
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) return NULL;

    zip_file_t *zf = zip_fopen(za, "MANIFEST.txt", 0);
    if (zf == NULL) { zip_close(za); return NULL; }

    // Read the manifest — it's always small (< 1 KB).
    char *buf = calloc(1, 2048);
    if (buf == NULL) { zip_fclose(zf); zip_close(za); return NULL; }

    zip_int64_t n = zip_fread(zf, buf, 2047);
    if (n < 0) { free(buf); buf = NULL; }

    zip_fclose(zf);
    zip_close(za);
    return buf;
}

// -------------------------------------------------------------------
// loadTagMap  –  build a lookup of commit zip → tag name by scanning
// .vc/tags/.  Fills tagNames[i] for up to maxTags entries.
// -------------------------------------------------------------------
typedef struct { char zip[256]; char tag[256]; } TagEntry;

static int loadTagMap(TagEntry *tags, int maxTags) {
    DIR *dir = opendir(".vc/tags");
    if (dir == NULL) return 0;

    int count = 0;
    struct dirent *entry;
    char path[MAX_DIR_PATH];
    char line[512];

    while (count < maxTags && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), ".vc/tags/%s", entry->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) continue;

        tags[count].zip[0] = '\0';
        snprintf(tags[count].tag, sizeof(tags[count].tag),
                 "%s", entry->d_name);

        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "commit", 6) == 0) {
                const char *p = line + 6;
                while (*p == ' ' || *p == '\t' || *p == '=') p++;
                size_t len = strlen(p);
                if (len > 0 && p[len-1] == '\n') len--;
                snprintf(tags[count].zip, sizeof(tags[count].zip),
                         "%.*s", (int)len, p);
                break;
            }
        }
        fclose(f);

        if (tags[count].zip[0] != '\0')
            count++;
    }
    closedir(dir);
    return count;
}

static int cmpOldestFirst(const void *a, const void *b) {
    const CommitInfo *ca = (const CommitInfo *)a;
    const CommitInfo *cb = (const CommitInfo *)b;
    return strcmp(ca->archive, cb->archive);  // ascending
}

// -------------------------------------------------------------------
// compareCommits  –  qsort comparator: sort by archive name (which is
// YYYYMMDD_HHMMSS.zip so lexicographic order = chronological order).
// -------------------------------------------------------------------
static int cmpNewestFirst(const void *a, const void *b) {
    const CommitInfo *ca = (const CommitInfo *)a;
    const CommitInfo *cb = (const CommitInfo *)b;
    return strcmp(cb->archive, ca->archive);  // descending
}

// -------------------------------------------------------------------
// formatSize  –  human-readable file size.
// -------------------------------------------------------------------
static void formatSize(long bytes, char *buf, size_t sz) {
    if (bytes >= 1024 * 1024)
        snprintf(buf, sz, "%4.1f MB", (double)bytes / (1024*1024));
    else if (bytes >= 1024)
        snprintf(buf, sz, "%4.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, sz, "%4ld  B", bytes);
}

// -------------------------------------------------------------------
// vcList  –  entry point called from vc.c.
//
// Usage:
//   vc list                 Show all commits, newest first
//   vc list --oldest        Show oldest first
//   vc list --branch <name> Show only commits tagged on a branch
//   vc list --count <n>     Show only the most recent n commits
// -------------------------------------------------------------------
int vcList(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // Parse flags.
    bool   oldestFirst  = false;
    int    limitCount   = 0;      // 0 = no limit

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--oldest") == 0) {
            oldestFirst = true;
        } else if (strcmp(argv[i], "--count") == 0 && i+1 < argc) {
            limitCount = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--count=", 8) == 0) {
            limitCount = atoi(argv[i] + 8);
        }
    }

    // 1. Scan .vc/data/ for zip files.
    DIR *dir = opendir(".vc/data");
    if (dir == NULL) {
        printf("No commits found.\n");
        return 0;
    }

    // Count zips first.
    int total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
            total++;
    }
    rewinddir(dir);

    if (total == 0) {
        closedir(dir);
        printf("No commits found.\n");
        return 0;
    }

    CommitInfo *commits = calloc((size_t)total, sizeof(CommitInfo));
    if (commits == NULL) { closedir(dir); return -1; }

    // 2. Load tag map for annotation.
    TagEntry tags[256];
    int tagCount = loadTagMap(tags, 256);

    // 3. Load each zip's manifest.
    int count = 0;
    char zipPath[MAX_DIR_PATH];

    while ((de = readdir(dir)) != NULL && count < total) {
        size_t len = strlen(de->d_name);
        if (len <= 4 || strcmp(de->d_name + len - 4, ".zip") != 0)
            continue;

        snprintf(zipPath, sizeof(zipPath), ".vc/data/%s", de->d_name);

        CommitInfo *ci = &commits[count];
        snprintf(ci->archive, sizeof(ci->archive), "%s", de->d_name);

        // Get zip file size from disk.
        struct stat st;
        if (stat(zipPath, &st) == 0)
            ci->sizeBytes = (long)st.st_size;

        // Read manifest.
        char *manifest = loadManifestFromZip(zipPath);
        if (manifest != NULL) {
            readManifestField(manifest, "date",    ci->date,    sizeof(ci->date));
            readManifestField(manifest, "author",  ci->author,  sizeof(ci->author));
            readManifestField(manifest, "project", ci->project, sizeof(ci->project));
            readManifestField(manifest, "message", ci->message, sizeof(ci->message));
            ci->fileCount = readManifestInt(manifest, "files");
            free(manifest);
        } else {
            snprintf(ci->date,    sizeof(ci->date),    "(unknown)");
            snprintf(ci->author,  sizeof(ci->author),  "(unknown)");
            snprintf(ci->message, sizeof(ci->message), "(no manifest)");
        }

        // Check if any tag(s) point to this archive — collect all of them.
        for (int t = 0; t < tagCount; t++) {
            if (strcmp(tags[t].zip, de->d_name) == 0) {
                if (!ci->hasTag) {
                    // First tag — store directly.
                    snprintf(ci->tagName, sizeof(ci->tagName),
                             "%s", tags[t].tag);
                    ci->hasTag = true;
                } else {
                    // Additional tags — append comma-separated.
                    size_t used = strlen(ci->tagName);
                    size_t rem  = sizeof(ci->tagName) - used;
                    if (rem > 2)
                        snprintf(ci->tagName + used, rem, ", %s", tags[t].tag);
                }
            }
        }

        count++;
    }
    closedir(dir);

    // 4. Sort.
    if (oldestFirst) {
        qsort(commits, (size_t)count, sizeof(CommitInfo), cmpOldestFirst);
    } else {
        qsort(commits, (size_t)count, sizeof(CommitInfo), cmpNewestFirst);
    }

    // Apply count limit.
    if (limitCount > 0 && limitCount < count)
        count = limitCount;

    // 5. Print header.
    extern Config *config;
    const char *project = (config && config->project[0])
                          ? config->project : "(unknown)";
    char *branch = vcBranchCurrentName();

    printf("\nCommit history — %s  (branch: %s)\n\n", project,
           branch ? branch : "main");
    free(branch);

    printf("  %-22s  %-5s  %-8s  %-24s  %s\n",
           "Archive", "Files", "Size", "Date", "Message");
    printf("  %-22s  %-5s  %-8s  %-24s  %s\n",
           "----------------------",
           "-----", "--------",
           "------------------------",
           "-------");

    // 6. Print each commit.
    for (int i = 0; i < count; i++) {
        CommitInfo *ci = &commits[i];
        char sizeBuf[16];
        formatSize(ci->sizeBytes, sizeBuf, sizeof(sizeBuf));

        // Truncate message to fit column.
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", ci->message);

        // Tag annotation.
        char tag[320] = "";
        if (ci->hasTag)
            snprintf(tag, sizeof(tag), " \033[0;33m[%s]\033[0m", ci->tagName);

        printf("  %-22s  %5d  %8s  %-24s  %s%s\n",
               ci->archive,
               ci->fileCount,
               sizeBuf,
               ci->date,
               msg,
               tag);
    }

    printf("\n  %d commit(s) shown.\n\n", count);

    free(commits);
    return 0;
}
