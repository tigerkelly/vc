#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <zip.h>

#include "vc.h"

// -------------------------------------------------------------------
// Types and helpers shared with vcList.c — replicated here to avoid
// exposing them as globals.
// -------------------------------------------------------------------
typedef struct {
    char archive[256];
    char date[32];
    char author[128];
    char project[128];
    char message[256];
    bool hasTag;
    char tagName[256];
    int  fileCount;
    long sizeBytes;
} CommitInfo;

typedef struct { char zip[256]; char tag[256]; } TagEntry;

static int cmpNewestFirst(const void *a, const void *b) {
    return strcmp(((CommitInfo*)b)->archive, ((CommitInfo*)a)->archive);
}

static char *loadManifestFromZip(const char *zipPath) {
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) return NULL;
    zip_file_t *zf = zip_fopen(za, "MANIFEST.txt", 0);
    if (zf == NULL) { zip_close(za); return NULL; }
    char *buf = calloc(1, 2048);
    if (buf == NULL) { zip_fclose(zf); zip_close(za); return NULL; }
    zip_int64_t n = zip_fread(zf, buf, 2047);
    if (n < 0) { free(buf); buf = NULL; }
    zip_fclose(zf);
    zip_close(za);
    return buf;
}

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
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
}

static int readManifestInt(const char *manifest, const char *field) {
    char buf[32];
    if (readManifestField(manifest, field, buf, sizeof(buf)))
        return atoi(buf);
    return 0;
}

static int loadTagMap(TagEntry *tags, int maxTags) {
    char tagDir[MAX_DIR_PATH];
    snprintf(tagDir, sizeof(tagDir), "%s/.vc/tags", vcTopDir);
    DIR *dir = opendir(tagDir);
    if (dir == NULL) return 0;
    int count = 0;
    struct dirent *entry;
    char path[MAX_DIR_PATH], line[512];
    while (count < maxTags && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", tagDir, entry->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) continue;
        tags[count].zip[0] = '\0';
        snprintf(tags[count].tag, sizeof(tags[count].tag), "%s", entry->d_name);
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
        if (tags[count].zip[0] != '\0') count++;
    }
    closedir(dir);
    return count;
}

#define VC_ARCHIVE_DIR  ".vc/archive"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static void ensureArchiveDir(void) {
    char path[MAX_DIR_PATH];
    snprintf(path, sizeof(path), "%s/%s", vcTopDir, VC_ARCHIVE_DIR);
    struct stat st;
    if (stat(path, &st) != 0)
        mkdir(path, 0755);
}

// Collect zip filenames from a directory into zips[].
// Returns count.  Caller provides zips[][64] and max.
static int collectZips(const char *dir, char zips[][256], int max) {
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < max) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
            snprintf(zips[count++], 256, "%s", de->d_name);
    }
    closedir(d);
    return count;
}

// Sort zips[] newest-first (lexicographic descending).
static void sortNewestFirst(char zips[][256], int count) {
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(zips[j], zips[i]) > 0) {
                char tmp[256];
                strncpy(tmp,     zips[i], 255); tmp[255]     = '\0';
                strncpy(zips[i], zips[j], 255); zips[i][255] = '\0';
                strncpy(zips[j], tmp,     255); zips[j][255] = '\0';
            }
}

static void formatSize(long bytes, char *buf, size_t sz) {
    if (bytes >= 1024 * 1024)
        snprintf(buf, sz, "%4.1f MB", (double)bytes / (1024*1024));
    else if (bytes >= 1024)
        snprintf(buf, sz, "%4.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, sz, "%4ld  B", bytes);
}

// -------------------------------------------------------------------
// cmdArchiveMove  –  move zips from data/ to archive/ (or vice versa).
// -------------------------------------------------------------------
static int moveZip(const char *zipName,
                   const char *fromDir,
                   const char *toDir,
                   const char *label) {
    char src[MAX_DIR_PATH];
    char dst[MAX_DIR_PATH];
    snprintf(src, sizeof(src), "%s/%s", fromDir, zipName);
    snprintf(dst, sizeof(dst), "%s/%s", toDir,   zipName);

    // Check destination doesn't already exist.
    struct stat st;
    if (stat(dst, &st) == 0) {
        fprintf(stderr, "  skip  %s  (already in %s)\n", zipName, label);
        return 0;
    }

    if (rename(src, dst) == 0) {
        char sizeBuf[16];
        if (stat(dst, &st) == 0)
            formatSize((long)st.st_size, sizeBuf, sizeof(sizeBuf));
        else
            snprintf(sizeBuf, sizeof(sizeBuf), "?");
        printf("  %-8s  %-30s  %s\n", label, zipName, sizeBuf);
        return 1;
    }

    // rename() can fail across filesystems — fall back to copy + delete.
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        fprintf(stderr, "  FAIL  %s: %s\n", zipName, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fprintf(stderr, "  FAIL  %s: cannot create destination: %s\n",
                zipName, strerror(errno));
        fclose(in);
        return -1;
    }

    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }

    fclose(in);
    fclose(out);

    if (!ok) {
        remove(dst);
        fprintf(stderr, "  FAIL  %s: write error\n", zipName);
        return -1;
    }
    remove(src);

    char sizeBuf[16];
    if (stat(dst, &st) == 0)
        formatSize((long)st.st_size, sizeBuf, sizeof(sizeBuf));
    else
        snprintf(sizeBuf, sizeof(sizeBuf), "?");
    printf("  %-8s  %-30s  %s\n", label, zipName, sizeBuf);
    return 1;
}

// -------------------------------------------------------------------
// cmdList  –  show archived commits in the same format as 'vc list'.
// -------------------------------------------------------------------
static int cmdList(void) {
    char archiveDir[MAX_DIR_PATH];
    snprintf(archiveDir, sizeof(archiveDir), "%s/%s", vcTopDir, VC_ARCHIVE_DIR);

    DIR *d = opendir(archiveDir);
    if (d == NULL) {
        printf("Archive is empty. Use 'vc archive' to move old commits here.\n");
        return 0;
    }

    // Count zips.
    int total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
            total++;
    }
    rewinddir(d);

    if (total == 0) {
        closedir(d);
        printf("Archive is empty. Use 'vc archive' to move old commits here.\n");
        return 0;
    }

    // Load tag map so archived commits that were tagged show the tag.
    TagEntry tags[256];
    int tagCount = loadTagMap(tags, 256);

    // Allocate commit info array.
    CommitInfo *commits = calloc((size_t)total, sizeof(CommitInfo));
    if (commits == NULL) { closedir(d); return -1; }

    int count = 0;
    char zipPath[MAX_DIR_PATH];

    while ((de = readdir(d)) != NULL && count < total) {
        size_t len = strlen(de->d_name);
        if (len <= 4 || strcmp(de->d_name + len - 4, ".zip") != 0)
            continue;

        snprintf(zipPath, sizeof(zipPath), "%s/%s", archiveDir, de->d_name);

        CommitInfo *ci = &commits[count];
        snprintf(ci->archive, sizeof(ci->archive), "%s", de->d_name);

        struct stat st;
        if (stat(zipPath, &st) == 0)
            ci->sizeBytes = (long)st.st_size;

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

        // Collect all tags pointing to this archive.
        for (int t = 0; t < tagCount; t++) {
            if (strcmp(tags[t].zip, de->d_name) == 0) {
                if (!ci->hasTag) {
                    snprintf(ci->tagName, sizeof(ci->tagName),
                             "%s", tags[t].tag);
                    ci->hasTag = true;
                } else {
                    size_t used = strlen(ci->tagName);
                    size_t rem  = sizeof(ci->tagName) - used;
                    if (rem > 2)
                        snprintf(ci->tagName + used, rem, ", %s", tags[t].tag);
                }
            }
        }
        count++;
    }
    closedir(d);

    // Sort newest-first.
    qsort(commits, (size_t)count, sizeof(CommitInfo), cmpNewestFirst);

    // Print header — same format as vcList.
    extern Config *config;
    const char *project = (config && config->project[0])
                          ? config->project : "(unknown)";
    char *branch = vcBranchCurrentName();

    printf("\nArchived commits — %s  (branch: %s)\n\n",
           project, branch ? branch : "main");
    free(branch);

    printf("  %-22s  %-5s  %-8s  %-24s  %s\n",
           "Archive", "Files", "Size", "Date", "Message");
    printf("  %-22s  %-5s  %-8s  %-24s  %s\n",
           "----------------------",
           "-----", "--------",
           "------------------------",
           "-------");

    for (int i = 0; i < count; i++) {
        CommitInfo *ci = &commits[i];
        char sizeBuf[16];
        formatSize(ci->sizeBytes, sizeBuf, sizeof(sizeBuf));

        char msg[256];
        snprintf(msg, sizeof(msg), "%s", ci->message);

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

    printf("\n  %d archived commit(s).\n\n", count);
    free(commits);
    return 0;
}

// -------------------------------------------------------------------
// cmdRestore  –  move a zip back from archive/ to data/.
// -------------------------------------------------------------------
static int cmdRestore(const char *zipName) {
    char archiveDir[MAX_DIR_PATH];
    char dataDir[MAX_DIR_PATH];
    snprintf(archiveDir, sizeof(archiveDir), "%s/%s", vcTopDir, VC_ARCHIVE_DIR);
    snprintf(dataDir,    sizeof(dataDir),    "%s/.vc/data", vcTopDir);

    // Accept bare name or full path.
    char name[64];
    const char *base = strrchr(zipName, '/');
    snprintf(name, sizeof(name), "%s", base ? base + 1 : zipName);

    // Verify it exists in archive.
    char src[MAX_DIR_PATH];
    snprintf(src, sizeof(src), "%s/%s", archiveDir, name);
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "vcArchive: '%s' not found in archive.\n", name);
        fprintf(stderr, "           Use 'vc archive --list' to see archived commits.\n");
        return -1;
    }

    printf("Restoring %s to active commits...\n", name);
    int rc = moveZip(name, archiveDir, dataDir, "restored");
    if (rc > 0)
        printf("\nRestored. Run 'vc list' to see updated commit history.\n");
    return rc > 0 ? 0 : -1;
}

// -------------------------------------------------------------------
// vcArchive  –  entry point called from vc.c.
//
// Usage:
//   vc archive                  Archive all commits except the most recent
//   vc archive --all            Archive every commit
//   vc archive --keep <n>       Keep n most recent commits, archive the rest
//   vc archive --list           Show archived commits
//   vc archive --restore <zip>  Move a zip back from archive to active
// -------------------------------------------------------------------
int vcArchive(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // Parse arguments.
    bool doList    = false;
    int  keepCount = 1;        // default: keep the most recent commit
    const char *restoreZip = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            keepCount = 0;
        } else if (strcmp(argv[i], "--list") == 0) {
            doList = true;
        } else if (strcmp(argv[i], "--restore") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "vcArchive: --restore requires a zip filename.\n");
                return -1;
            }
            restoreZip = argv[++i];
        } else if (strcmp(argv[i], "--keep") == 0) {
            if (i + 1 >= argc || atoi(argv[i+1]) < 0) {
                fprintf(stderr, "vcArchive: --keep requires a non-negative number.\n");
                return -1;
            }
            keepCount = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--keep=", 7) == 0) {
            keepCount = atoi(argv[i] + 7);
        } else {
            fprintf(stderr, "vcArchive: unknown option '%s'\n"
                            "           Usage: vc archive [--all|--keep <n>|"
                            "--list|--restore <zip>]\n", argv[i]);
            return -1;
        }
    }

    ensureArchiveDir();

    if (doList)
        return cmdList();

    if (restoreZip)
        return cmdRestore(restoreZip);

    // Collect active zips from .vc/data/, sorted newest-first.
    char dataDir[MAX_DIR_PATH];
    char archiveDir[MAX_DIR_PATH];
    snprintf(dataDir,    sizeof(dataDir),    "%s/.vc/data", vcTopDir);
    snprintf(archiveDir, sizeof(archiveDir), "%s/%s", vcTopDir, VC_ARCHIVE_DIR);

    char zips[512][256];
    int total = collectZips(dataDir, zips, 512);

    if (total == 0) {
        printf("No commits to archive.\n");
        return 0;
    }

    sortNewestFirst(zips, total);

    // How many to archive?
    int toArchive = total - keepCount;
    if (toArchive <= 0) {
        printf("Nothing to archive — only %d commit(s) present, keeping %d.\n",
               total, keepCount);
        return 0;
    }

    printf("Archiving %d of %d commit(s) (keeping %d most recent)...\n\n",
           toArchive, total, keepCount);
    printf("  %-8s  %-30s  %s\n", "Action", "Archive", "Size");
    printf("  %-8s  %-30s  %s\n",
           "--------", "------------------------------", "-------");

    int archived = 0;
    // Archive oldest commits (end of sorted array).
    for (int i = keepCount; i < total; i++) {
        int rc = moveZip(zips[i], dataDir, archiveDir, "archived");
        if (rc > 0) archived++;
    }

    printf("\n%d commit(s) archived to %s/\n", archived, VC_ARCHIVE_DIR);
    printf("Run 'vc archive --list' to see archived commits.\n");
    printf("Run 'vc archive --restore <zip>' to bring one back.\n");
    return 0;
}
