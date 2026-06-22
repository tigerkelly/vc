
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "vc.h"

// -------------------------------------------------------------------
// removeDir  –  recursively remove a directory and all its contents.
// Returns true on success.
// -------------------------------------------------------------------
static bool removeDir(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) return false;

    struct dirent *entry;
    struct stat st;
    char child[MAX_DIR_PATH];
    bool ok = true;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        if (stat(child, &st) == -1) { ok = false; continue; }

        if (S_ISDIR(st.st_mode)) {
            if (!removeDir(child)) ok = false;
        } else {
            if (remove(child) != 0) {
                fprintf(stderr, "vcDelete: cannot remove '%s': %s\n",
                        child, strerror(errno));
                ok = false;
            }
        }
    }
    closedir(dir);

    if (ok && rmdir(path) != 0) {
        fprintf(stderr, "vcDelete: cannot remove directory '%s': %s\n",
                path, strerror(errno));
        ok = false;
    }
    return ok;
}

// -------------------------------------------------------------------
// removeIndexEntries  –  remove one file or all entries under a
// directory prefix from the index.
// Returns the number of entries removed.
// -------------------------------------------------------------------
static int removeIndexEntries(IndexEntry *entries, int *count,
                               const char *relPath) {
    bool isDir = (relPath[strlen(relPath)-1] == '/');
    size_t prefixLen = strlen(relPath);
    int removed = 0;

    for (int i = 0; i < *count; ) {
        bool match = false;

        if (strcmp(entries[i].path, relPath) == 0) {
            // Exact match (file or dir entry itself).
            match = true;
        } else if (isDir &&
                   strncmp(entries[i].path, relPath, prefixLen) == 0) {
            // File inside the directory being deleted.
            match = true;
        }

        if (match) {
            free(entries[i].path);
            // Shift remaining entries down.
            for (int j = i; j < *count - 1; j++)
                entries[j] = entries[j+1];
            (*count)--;
            removed++;
            // Don't increment i — the next entry is now at position i.
        } else {
            i++;
        }
    }
    return removed;
}

// -------------------------------------------------------------------
// deleteOne  –  delete a single path (file or directory).
//
// keepOnDisk: if true, only remove from tracking; leave file intact.
// force:      if true, delete from disk without prompting.
// -------------------------------------------------------------------
static int deleteOne(const char *userPath, bool keepOnDisk, bool force) {

    // Build full and relative paths.
    char fullPath[MAX_DIR_PATH];
    if (userPath[0] == '/')
        snprintf(fullPath, sizeof(fullPath), "%s", userPath);
    else
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, userPath);

    // Build the canonical relative path.
    char relPath[MAX_DIR_PATH];
    char canonTop[MAX_DIR_PATH];
    char canonical[MAX_DIR_PATH];

    if (realpath(vcTopDir, canonTop) == NULL)
        snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);

    if (realpath(fullPath, canonical) != NULL) {
        size_t topLen = strlen(canonTop);
        if (strncmp(canonical, canonTop, topLen) == 0) {
            const char *rel = canonical + topLen;
            if (*rel == '/') rel++;
            snprintf(relPath, sizeof(relPath), "%s", rel);
        } else {
            snprintf(relPath, sizeof(relPath), "%s", userPath);
        }
    } else {
        // Path doesn't exist on disk — strip prefix manually.
        size_t topLen = strlen(vcTopDir);
        if (strncmp(fullPath, vcTopDir, topLen) == 0) {
            const char *rel = fullPath + topLen;
            if (*rel == '/') rel++;
            snprintf(relPath, sizeof(relPath), "%s", rel);
        } else {
            snprintf(relPath, sizeof(relPath), "%s", userPath);
        }
    }

    // Detect whether this is a directory path.
    struct stat st;
    bool existsOnDisk = (stat(fullPath, &st) == 0);
    bool isDir = existsOnDisk && S_ISDIR(st.st_mode);

    // For the index lookup, directory paths use a trailing slash.
    char indexKey[MAX_DIR_PATH];
    if (isDir) {
        size_t rlen = strlen(relPath);
        if (rlen > 0 && relPath[rlen-1] == '/')
            snprintf(indexKey, sizeof(indexKey), "%s", relPath);
        else
            snprintf(indexKey, sizeof(indexKey), "%s/", relPath);
    } else {
        snprintf(indexKey, sizeof(indexKey), "%s", relPath);
    }

    // Load the index and verify the path is tracked.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);
    if (count < 0) {
        fprintf(stderr, "vcDelete: failed to load index.\n");
        return -1;
    }

    int idx = vcIndexFind(entries, count, indexKey);
    bool isTracked = (idx >= 0);

    // Also check for files inside a directory even if the dir entry
    // itself isn't in the index.
    if (!isTracked && isDir) {
        for (int i = 0; i < count; i++) {
            if (strncmp(entries[i].path, indexKey, strlen(indexKey)) == 0) {
                isTracked = true;
                break;
            }
        }
    }

    if (!isTracked && !existsOnDisk) {
        fprintf(stderr, "vcDelete: '%s' is not tracked and does not exist on disk.\n",
                userPath);
        vcIndexFree(entries, count);
        return -1;
    }

    // Prompt for confirmation unless --force was given.
    if (!force && !keepOnDisk && existsOnDisk) {
        printf("Delete '%s' from disk and tracking? [y/N]: ", userPath);
        fflush(stdout);
        char answer[8];
        if (fgets(answer, sizeof(answer), stdin) == NULL ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("Aborted.\n");
            vcIndexFree(entries, count);
            return 0;
        }
    }

    // Remove from the index.
    int removed = removeIndexEntries(entries, &count, indexKey);

    if (removed > 0) {
        if (vcIndexSave(entries, count) != 0) {
            fprintf(stderr, "vcDelete: warning – could not save index.\n");
        }
    }

    vcIndexFree(entries, count);

    // Remove from disk unless --keep was specified.
    if (!keepOnDisk && existsOnDisk) {
        bool diskOk;
        if (isDir)
            diskOk = removeDir(fullPath);
        else
            diskOk = (remove(fullPath) == 0);

        if (!diskOk && !isDir) {
            fprintf(stderr, "vcDelete: could not remove '%s' from disk: %s\n",
                    userPath, strerror(errno));
            return -1;
        }

        if (removed > 0)
            printf("Deleted  : %s (%d index %s removed)\n",
                   userPath, removed, removed == 1 ? "entry" : "entries");
        else
            printf("Deleted  : %s (was not tracked)\n", userPath);
    } else {
        // --keep: only removed from tracking.
        if (removed > 0)
            printf("Untracked: %s (%d index %s removed, file kept on disk)\n",
                   userPath, removed, removed == 1 ? "entry" : "entries");
        else
            printf("Note: '%s' was not in the index.\n", userPath);
    }

    vcLog("delete: %s  keepOnDisk=%d\n", indexKey, keepOnDisk);
    return 0;
}

// -------------------------------------------------------------------
// vcDelete  –  entry point called from vc.c.
//
// Usage:
//   vc delete <file> [<file2> ...]         delete from disk and tracking
//   vc delete --keep <file> [<file2> ...]  remove from tracking only
//   vc delete --force <file> [<file2> ...] delete without prompting
// -------------------------------------------------------------------
int vcDelete(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    if (argc < 3) {
        fprintf(stderr, "vcDelete: usage: vc delete [--keep|--force] "
                        "<file> [<file2> ...]\n");
        return -1;
    }

    bool keepOnDisk = false;
    bool force      = false;
    int  firstFile  = 2;   // index in argv where filenames start

    // Parse flags.
    if (strcmp(argv[2], "--keep") == 0) {
        keepOnDisk = true;
        firstFile  = 3;
    } else if (strcmp(argv[2], "--force") == 0) {
        force     = true;
        firstFile = 3;
    }

    if (firstFile >= argc) {
        fprintf(stderr, "vcDelete: no files specified.\n");
        return -1;
    }

    int rc = 0;
    for (int i = firstFile; i < argc; i++) {
        if (deleteOne(argv[i], keepOnDisk, force) != 0)
            rc = -1;
    }
    return rc;
}
