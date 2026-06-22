
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "vc.h"

// -------------------------------------------------------------------
// buildRelPath  –  given a user-supplied path (absolute or relative
// to cwd, which is vcTopDir), return its canonical relative path
// the same way vcIndex stores it.
// e.g.  "src/main.c"  →  "src/main.c"
//        "./src/main.c" →  "src/main.c"
//        "/project/src/main.c" → "src/main.c"
// Fills buf and returns buf, or NULL on failure.
// -------------------------------------------------------------------
static const char *buildRelPath(const char *userPath, char *buf, size_t sz) {
    // Build full path.
    char fullPath[MAX_DIR_PATH];
    if (userPath[0] == '/') {
        snprintf(fullPath, sizeof(fullPath), "%s", userPath);
    } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, userPath);
    }

    // Use realpath if the file exists, otherwise normalise manually.
    char canonical[MAX_DIR_PATH];
    if (realpath(fullPath, canonical) != NULL) {
        // Strip vcTopDir prefix.
        char canonTop[MAX_DIR_PATH];
        if (realpath(vcTopDir, canonTop) == NULL)
            snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);

        size_t topLen = strlen(canonTop);
        if (strncmp(canonical, canonTop, topLen) == 0) {
            const char *rel = canonical + topLen;
            if (*rel == '/') rel++;
            snprintf(buf, sz, "%s", rel);
            return buf;
        }
        snprintf(buf, sz, "%s", canonical);
        return buf;
    }

    // File doesn't exist yet (destination path) – strip prefix manually.
    size_t topLen = strlen(vcTopDir);
    if (strncmp(fullPath, vcTopDir, topLen) == 0) {
        const char *rel = fullPath + topLen;
        if (*rel == '/') rel++;
        // Collapse any "./" prefix.
        while (strncmp(rel, "./", 2) == 0) rel += 2;
        snprintf(buf, sz, "%s", rel);
        return buf;
    }

    snprintf(buf, sz, "%s", userPath);
    return buf;
}

// -------------------------------------------------------------------
// vcRename  –  rename a tracked file or directory.
//
// Usage:
//   vc rename <old> <new>
//
// What it does:
//   1. Validates old exists on disk and new does not.
//   2. Renames the file/directory on disk using rename(2).
//   3. Updates the index:
//        - Exact match on oldRel → update path to newRel, mark staged.
//        - For directories: any entry whose path starts with oldRel/
//          gets its prefix replaced with newRel/.
// -------------------------------------------------------------------
int vcRename(char *oldName, char *newName) {

    vcLog("%s %s\n", __func__, vcTopDir);

    if (oldName == NULL || newName == NULL ||
        oldName[0] == '\0' || newName[0] == '\0') {
        fprintf(stderr, "vcRename: usage: vc rename <old> <new>\n");
        return -1;
    }

    // Build relative paths for index lookups.
    char oldRel[MAX_DIR_PATH];
    char newRel[MAX_DIR_PATH];

    buildRelPath(oldName, oldRel, sizeof(oldRel));
    buildRelPath(newName, newRel, sizeof(newRel));

    if (oldRel[0] == '\0' || newRel[0] == '\0') {
        fprintf(stderr, "vcRename: could not resolve paths.\n");
        return -1;
    }

    // Build full disk paths.
    char oldFull[MAX_DIR_PATH];
    char newFull[MAX_DIR_PATH];
    snprintf(oldFull, sizeof(oldFull), "%s/%s", vcTopDir, oldRel);
    snprintf(newFull, sizeof(newFull), "%s/%s", vcTopDir, newRel);

    // 1. Validate.
    struct stat oldSt;
    if (stat(oldFull, &oldSt) == -1) {
        fprintf(stderr, "vcRename: '%s' does not exist.\n", oldName);
        return -1;
    }

    struct stat newSt;
    if (stat(newFull, &newSt) == 0) {
        fprintf(stderr, "vcRename: '%s' already exists.\n", newName);
        return -1;
    }

    bool isDir = S_ISDIR(oldSt.st_mode);

    // 2. Rename on disk.
    if (rename(oldFull, newFull) != 0) {
        fprintf(stderr, "vcRename: cannot rename '%s' to '%s': %s\n",
                oldName, newName, strerror(errno));
        return -1;
    }

    printf("Renamed: %s  →  %s\n", oldRel, newRel);

    // 3. Load the index and update it.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);
    if (count < 0) {
        fprintf(stderr, "vcRename: warning – could not load index.\n");
        return 0;   // disk rename succeeded; index warn is non-fatal
    }

    int updated = 0;

    if (isDir) {
        // Directory rename: update the directory entry itself (oldRel/)
        // AND any file entries whose path starts with oldRel/.
        char oldDirKey[MAX_DIR_PATH];
        char newDirKey[MAX_DIR_PATH];
        snprintf(oldDirKey, sizeof(oldDirKey), "%s/", oldRel);
        snprintf(newDirKey, sizeof(newDirKey), "%s/", newRel);
        size_t oldDirLen = strlen(oldDirKey);

        for (int i = 0; i < count; i++) {
            const char *p = entries[i].path;

            if (strcmp(p, oldDirKey) == 0) {
                // The directory entry itself.
                free(entries[i].path);
                entries[i].path  = strdup(newDirKey);
                entries[i].state = INDEX_STAGED;
                updated++;
                printf("  updated index: %s  →  %s\n", oldDirKey, newDirKey);

            } else if (strncmp(p, oldDirKey, oldDirLen) == 0) {
                // A file inside the renamed directory.
                char newPath[MAX_DIR_PATH];
                snprintf(newPath, sizeof(newPath), "%s/%s",
                         newRel, p + oldDirLen);
                printf("  updated index: %s  →  %s\n", p, newPath);
                free(entries[i].path);
                entries[i].path  = strdup(newPath);
                entries[i].state = INDEX_STAGED;
                updated++;
            }
        }
    } else {
        // File rename: find the exact entry.
        int idx = vcIndexFind(entries, count, oldRel);
        if (idx >= 0) {
            free(entries[idx].path);
            entries[idx].path  = strdup(newRel);
            entries[idx].state = INDEX_STAGED;
            updated++;
            printf("  updated index: %s  →  %s\n", oldRel, newRel);
        } else {
            // File wasn't tracked — add the new name as a new staged entry.
            printf("  note: '%s' was not tracked; adding '%s' as new.\n",
                   oldRel, newRel);
            entries = vcIndexStage(entries, &count, newFull);
            updated++;
        }
    }

    // 4. Save the updated index.
    if (updated > 0) {
        if (vcIndexSave(entries, count) != 0) {
            fprintf(stderr, "vcRename: warning – could not save index.\n");
        } else {
            printf("%d index entry/entries updated.\n", updated);
        }
    } else {
        printf("  note: no index entries matched '%s'.\n", oldRel);
    }

    vcIndexFree(entries, count);
    return 0;
}
