
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "vc.h"

// -------------------------------------------------------------------
// mkdirParents  –  create all intermediate directories in path,
// equivalent to  mkdir -p  on the parent of the given file path.
// Returns true on success (or if they already exist).
// -------------------------------------------------------------------
static bool mkdirParents(const char *filePath) {
    char tmp[MAX_DIR_PATH];
    snprintf(tmp, sizeof(tmp), "%s", filePath);

    // Walk forward and mkdir each component.
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "vcCopy: cannot create directory '%s': %s\n",
                        tmp, strerror(errno));
                return false;
            }
            *p = '/';
        }
    }
    return true;
}

// -------------------------------------------------------------------
// vcCopy  –  copy a source file into the vc data store, preserving
// its relative path so files from different directories never collide.
//
// Source : <vcTopDir>/src/utils/parser.c
// Dest   : <vcTopDir>/.vc/data/src/utils/parser.c
//
// Returns 0 on success, -1 on any error.
// -------------------------------------------------------------------
int vcCopy(char *path) {

    vcLog("%s %s\n", __func__, path);

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "vcCopy: no path supplied.\n");
        return -1;
    }

    // 1. Compute the relative path by stripping vcTopDir prefix.
    const char *relPath = path;
    size_t topLen = strlen(vcTopDir);
    if (strncmp(path, vcTopDir, topLen) == 0) {
        relPath = path + topLen;
        if (*relPath == '/') relPath++;
    }

    if (relPath[0] == '\0') {
        fprintf(stderr, "vcCopy: path resolves to empty relative path: %s\n",
                path);
        return -1;
    }

    // 2. Build the destination path preserving directory structure.
    char dstPath[MAX_DIR_PATH];
    snprintf(dstPath, sizeof(dstPath), "%s/.vc/%s/%s",
             vcTopDir, VC_DATA_DIR, relPath);

    // 3. Create all intermediate directories in the destination.
    if (!mkdirParents(dstPath))
        return -1;

    // 4. Open source in binary mode — never use text mode for file copies.
    FILE *src = fopen(path, "rb");
    if (src == NULL) {
        fprintf(stderr, "vcCopy: cannot open source '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    // 5. Write to a temp file first, then atomically rename to destination.
    //    This ensures no half-written file is left if the process is killed.
    char tmpPath[MAX_DIR_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", dstPath);

    FILE *dst = fopen(tmpPath, "wb");
    if (dst == NULL) {
        fprintf(stderr, "vcCopy: cannot open destination '%s': %s\n",
                tmpPath, strerror(errno));
        fclose(src);
        return -1;
    }

    // 6. Copy in chunks, checking fwrite for errors.
    char buffer[65536];   // 64 KB — large enough for efficiency
    size_t bytesRead;
    bool writeError = false;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        size_t written = fwrite(buffer, 1, bytesRead, dst);
        if (written != bytesRead) {
            fprintf(stderr, "vcCopy: write error on '%s': %s\n",
                    tmpPath, strerror(errno));
            writeError = true;
            break;
        }
    }

    // Check for read errors too.
    if (!writeError && ferror(src)) {
        fprintf(stderr, "vcCopy: read error on '%s': %s\n",
                path, strerror(errno));
        writeError = true;
    }

    fclose(src);
    fclose(dst);

    if (writeError) {
        remove(tmpPath);
        return -1;
    }

    // 7. Validate: compare source and destination sizes.
    struct stat srcSt, dstSt;
    if (stat(path, &srcSt) == 0 && stat(tmpPath, &dstSt) == 0) {
        if (srcSt.st_size != dstSt.st_size) {
            fprintf(stderr, "vcCopy: size mismatch after copy "
                            "(src=%lld dst=%lld) — aborting.\n",
                    (long long)srcSt.st_size, (long long)dstSt.st_size);
            remove(tmpPath);
            return -1;
        }
    }

    // 8. Atomic rename temp → final destination.
    if (rename(tmpPath, dstPath) != 0) {
        fprintf(stderr, "vcCopy: cannot rename '%s' to '%s': %s\n",
                tmpPath, dstPath, strerror(errno));
        remove(tmpPath);
        return -1;
    }

    return 0;
}
