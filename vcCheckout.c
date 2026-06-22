
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <zip.h>

#include "vc.h"

// -------------------------------------------------------------------
// vcCheckout  –  restore files from a commit archive.
//
// Usage:
//   vc checkout <file>           Restore one file from the last commit
//   vc checkout --all            Restore all files from the last commit
//   vc checkout --tag <name>     Restore entire project to a tagged commit
//   vc checkout --zip <zipfile>  Restore entire project to a specific zip
//
// "Restore" means: overwrite the working copy with the version stored
// in the commit archive and update the index mtime/size so the file
// does not appear modified afterwards.
//
// checkout never deletes files that are not in the archive — it only
// writes files it finds there.  Use 'vc branch' to do a full switch.
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// resolveRelPath  –  same logic as vcRename's buildRelPath but for
// paths that must already exist on disk.
// -------------------------------------------------------------------
static const char *resolveRelPath(const char *userPath,
                                   char *buf, size_t sz) {
    char fullPath[MAX_DIR_PATH];
    if (userPath[0] == '/')
        snprintf(fullPath, sizeof(fullPath), "%s", userPath);
    else
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, userPath);

    char canonical[MAX_DIR_PATH];
    char canonTop[MAX_DIR_PATH];

    if (realpath(vcTopDir, canonTop) == NULL)
        snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);

    if (realpath(fullPath, canonical) != NULL) {
        size_t topLen = strlen(canonTop);
        if (strncmp(canonical, canonTop, topLen) == 0) {
            const char *rel = canonical + topLen;
            if (*rel == '/') rel++;
            snprintf(buf, sz, "%s", rel);
            return buf;
        }
    }

    // Fall back to simple prefix strip.
    size_t topLen = strlen(vcTopDir);
    if (strncmp(fullPath, vcTopDir, topLen) == 0) {
        const char *rel = fullPath + topLen;
        if (*rel == '/') rel++;
        snprintf(buf, sz, "%s", rel);
        return buf;
    }

    snprintf(buf, sz, "%s", userPath);
    return buf;
}

// -------------------------------------------------------------------
// refreshIndexEntry  –  after restoring a file, update its mtime and
// size in the index so vcStatus doesn't flag it as modified.
// -------------------------------------------------------------------
static void refreshIndexEntry(IndexEntry *entries, int count,
                               const char *relPath) {
    int idx = vcIndexFind(entries, count, relPath);
    if (idx < 0) return;

    char fullPath[MAX_DIR_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, relPath);

    struct stat st;
    if (stat(fullPath, &st) == 0) {
        entries[idx].mtime = st.st_mtime;
        entries[idx].size  = st.st_size;
        entries[idx].state = INDEX_COMMITTED;
    }
}

// -------------------------------------------------------------------
// checkoutFile  –  restore a single file from a zip archive.
// Updates the in-memory index entry so the file appears clean.
// -------------------------------------------------------------------
static int checkoutFile(const char *zipPath, const char *relPath,
                         IndexEntry *entries, int count) {
    if (!vcRestoreFileFromZip(zipPath, relPath)) {
        fprintf(stderr, "vcCheckout: '%s' not found in archive.\n", relPath);
        return -1;
    }

    refreshIndexEntry(entries, count, relPath);
    printf("  restored  %s\n", relPath);
    return 0;
}

// -------------------------------------------------------------------
// checkoutAll  –  restore every tracked file from commit archives.
// Iterates zips newest-first so each file gets its most recent committed
// version.  A file is only restored once (the first/newest zip that has it).
// -------------------------------------------------------------------
static int checkoutAll(const char *zipPath,
                        IndexEntry *entries, int count) {
    // Build list of all zips in .vc/data/, sorted newest-first.
    char dataDir[MAX_DIR_PATH];
    snprintf(dataDir, sizeof(dataDir), "%s/.vc/data", vcTopDir);

    DIR *dir = opendir(dataDir);
    if (dir == NULL) {
        fprintf(stderr, "vcCheckout: cannot open data dir '%s'\n", dataDir);
        return -1;
    }

    // Collect zip filenames.
    char zips[512][256];
    int  zipCount = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && zipCount < 512) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0) {
            snprintf(zips[zipCount], 256, "%s", de->d_name);
            zipCount++;
        }
    }
    closedir(dir);

    // Sort newest-first (lexicographic descending = chronological descending).
    for (int i = 0; i < zipCount - 1; i++) {
        for (int j = i + 1; j < zipCount; j++) {
            if (strcmp(zips[j], zips[i]) > 0) {
                char tmp[256];
                strncpy(tmp, zips[i], 255); tmp[255] = '\0';
                strncpy(zips[i], zips[j], 255); zips[i][255] = '\0';
                strncpy(zips[j], tmp,     255); zips[j][255] = '\0';
            }
        }
    }

    // Track which index entries have already been restored.
    bool restored[4096];
    memset(restored, 0, sizeof(bool) * (count < 4096 ? count : 4096));
    int totalRestored = 0;

    for (int z = 0; z < zipCount; z++) {
        char zpath[MAX_DIR_PATH];
        snprintf(zpath, sizeof(zpath), "%s/%s", dataDir, zips[z]);

        int err = 0;
        zip_t *za = zip_open(zpath, ZIP_RDONLY, &err);
        if (za == NULL) continue;

        zip_int64_t n = zip_get_num_entries(za, 0);

        for (zip_int64_t i = 0; i < n; i++) {
            const char *name = zip_get_name(za, i, 0);
            if (name == NULL) continue;
            if (strcmp(name, "MANIFEST.txt") == 0) continue;
            size_t nlen = strlen(name);
            if (nlen > 0 && name[nlen-1] == '/') continue;

            // Copy name before closing the zip — zip_get_name returns
            // a pointer into libzip's internal buffer, which is freed
            // when zip_close() is called.
            char nameCopy[MAX_DIR_PATH];
            snprintf(nameCopy, sizeof(nameCopy), "%s", name);

            // Find the index entry for this file.
            // If count == 0 (e.g. fresh clone), restore everything.
            int idx = vcIndexFind(entries, count, nameCopy);
            if (count > 0 && idx < 0) continue;  // not tracked — skip
            if (count > 0 && idx < 4096 && restored[idx]) continue; // already restored

            zip_close(za);
            if (vcRestoreFileFromZip(zpath, nameCopy)) {
                refreshIndexEntry(entries, count, nameCopy);
                printf("  restored  %s\n", nameCopy);
                if (count > 0 && idx >= 0 && idx < 4096)
                    restored[idx] = true;
                totalRestored++;
            }
            za = zip_open(zpath, ZIP_RDONLY, &err);
            if (za == NULL) break;
        }
        if (za) zip_close(za);
    }

    printf("\n%d file(s) restored.\n", totalRestored);
    return 0;
}

// -------------------------------------------------------------------
// zipPathForTag  –  look up a tag name and return the zip path it
// points to.  Fills buf and returns true on success.
// -------------------------------------------------------------------
static bool zipPathForTag(const char *tagName, char *buf, size_t sz) {
    char tagFile[MAX_DIR_PATH];
    snprintf(tagFile, sizeof(tagFile), ".vc/tags/%s", tagName);

    FILE *f = fopen(tagFile, "r");
    if (f == NULL) return false;

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "commit", 6) == 0) {
            // Parse "commit = <zipname>"
            const char *p = line + 6;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '\n') len--;
            snprintf(buf, sz, ".vc/data/%.*s", (int)len, p);
            found = true;
            break;
        }
    }
    fclose(f);

    if (!found) return false;

    struct stat st;
    return (stat(buf, &st) == 0);
}

// -------------------------------------------------------------------
// vcCheckout  –  entry point called from vc.c.
// -------------------------------------------------------------------
int vcCheckout(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    if (argc < 3) {
        fprintf(stderr, "vcCheckout: usage:\n"
                        "  vc checkout <file>           restore file from last commit\n"
                        "  vc checkout --all            restore all files from last commit\n"
                        "  vc checkout --tag <name>     restore to tagged commit\n"
                        "  vc checkout --zip <zipfile>  restore to specific zip\n");
        return -1;
    }

    // Load index so we can refresh entries after restoration.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);
    if (count < 0) count = 0;

    int rc = 0;

    // --all  –  restore everything from the last commit.
    if (strcmp(argv[2], "--all") == 0) {
        char zipPath[MAX_DIR_PATH];
        if (!vcBranchCommitZip(zipPath, sizeof(zipPath))) {
            fprintf(stderr, "vcCheckout: no commits found on this branch.\n");
            rc = -1;
        } else {
            printf("Restoring all files from %s ...\n", zipPath);
            rc = checkoutAll(zipPath, entries, count);
        }

    // --tag <name>  –  restore to a tagged commit.
    } else if (strcmp(argv[2], "--tag") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcCheckout: --tag requires a tag name.\n");
            rc = -1;
        } else {
            char zipPath[MAX_DIR_PATH];
            if (!zipPathForTag(argv[3], zipPath, sizeof(zipPath))) {
                fprintf(stderr, "vcCheckout: tag '%s' not found or its "
                                "commit archive is missing.\n", argv[3]);
                rc = -1;
            } else {
                printf("Restoring all files from tag '%s' (%s) ...\n",
                       argv[3], zipPath);
                rc = checkoutAll(zipPath, entries, count);
            }
        }

    // --zip <zipfile>  –  restore to a named zip.
    } else if (strcmp(argv[2], "--zip") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcCheckout: --zip requires a zip filename.\n");
            rc = -1;
        } else {
            char zipPath[MAX_DIR_PATH];
            // Accept bare filename or full path.
            if (strchr(argv[3], '/') != NULL)
                snprintf(zipPath, sizeof(zipPath), "%s", argv[3]);
            else
                snprintf(zipPath, sizeof(zipPath), ".vc/data/%s", argv[3]);

            struct stat st;
            if (stat(zipPath, &st) != 0) {
                fprintf(stderr, "vcCheckout: archive '%s' not found.\n",
                        zipPath);
                rc = -1;
            } else {
                printf("Restoring all files from %s ...\n", zipPath);
                rc = checkoutAll(zipPath, entries, count);
            }
        }

    // <file>  –  restore a single file from the last commit.
    } else {
        char relPath[MAX_DIR_PATH];
        resolveRelPath(argv[2], relPath, sizeof(relPath));

        // Check the file is tracked.
        if (vcIndexFind(entries, count, relPath) < 0) {
            fprintf(stderr, "vcCheckout: '%s' is not tracked by vc.\n"
                            "            Only tracked files can be checked out.\n",
                    argv[2]);
            rc = -1;
        } else {
            char zipPath[MAX_DIR_PATH];
            if (!vcBranchCommitZip(zipPath, sizeof(zipPath))) {
                fprintf(stderr, "vcCheckout: no commits found on this branch.\n");
                rc = -1;
            } else {
                rc = checkoutFile(zipPath, relPath, entries, count);
            }
        }
    }

    // Save refreshed index.
    if (rc == 0 && count > 0)
        vcIndexSave(entries, count);

    vcIndexFree(entries, count);
    return rc;
}
