
#include <stdio.h>
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
// extractToTemp  –  extract relPath from zipPath into a temp file.
// Returns the temp file path (heap-allocated, caller must free),
// or NULL on failure.
// -------------------------------------------------------------------
static char *extractToTemp(const char *zipPath, const char *relPath) {
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) {
        fprintf(stderr, "vcDiff: cannot open archive '%s'\n", zipPath);
        return NULL;
    }

    zip_file_t *zf = zip_fopen(za, relPath, 0);
    if (zf == NULL) {
        zip_close(za);
        return NULL;
    }

    // Build a temp file path.
    char *tmpPath = malloc(MAX_DIR_PATH);
    if (tmpPath == NULL) { zip_fclose(zf); zip_close(za); return NULL; }

    // Use the filename part so the diff header looks clean.
    const char *base = strrchr(relPath, '/');
    base = base ? base + 1 : relPath;
    snprintf(tmpPath, MAX_DIR_PATH, "/tmp/vcdiff_%d_%s", (int)getpid(), base);

    FILE *f = fopen(tmpPath, "wb");
    if (f == NULL) {
        fprintf(stderr, "vcDiff: cannot create temp file '%s': %s\n",
                tmpPath, strerror(errno));
        free(tmpPath);
        zip_fclose(zf);
        zip_close(za);
        return NULL;
    }

    char buf[65536];
    zip_int64_t n;
    while ((n = zip_fread(zf, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, f);

    fclose(f);
    zip_fclose(zf);
    zip_close(za);
    return tmpPath;
}

// -------------------------------------------------------------------
// findLatestZipForFile  –  scan .vc/data/ newest-first and return
// the path of the first zip that contains relPath.
// Returns heap-allocated path, or NULL if not found in any zip.
// -------------------------------------------------------------------
static char *findLatestZipForFile(const char *relPath) {
    char dataDir[MAX_DIR_PATH];
    snprintf(dataDir, sizeof(dataDir), "%s/.vc/data", vcTopDir);

    DIR *dir = opendir(dataDir);
    if (dir == NULL) return NULL;

    // Collect zip names.
    char zips[512][256];
    int  zipCount = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && zipCount < 512) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
            snprintf(zips[zipCount++], 256, "%s", de->d_name);
    }
    closedir(dir);

    // Sort newest-first (lexicographic descending).
    for (int i = 0; i < zipCount - 1; i++)
        for (int j = i + 1; j < zipCount; j++)
            if (strcmp(zips[j], zips[i]) > 0) {
                char tmp[256];
                strncpy(tmp,     zips[i], 255); tmp[255]     = '\0';
                strncpy(zips[i], zips[j], 255); zips[i][255] = '\0';
                strncpy(zips[j], tmp,     255); zips[j][255] = '\0';
            }

    // Find the newest zip containing relPath.
    for (int i = 0; i < zipCount; i++) {
        char zipPath[MAX_DIR_PATH];
        snprintf(zipPath, sizeof(zipPath), "%s/%s", dataDir, zips[i]);

        int err = 0;
        zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
        if (za == NULL) continue;

        zip_int64_t idx = zip_name_locate(za, relPath, 0);
        zip_close(za);

        if (idx >= 0) {
            char *result = malloc(MAX_DIR_PATH);
            if (result)
                snprintf(result, MAX_DIR_PATH, "%s", zipPath);
            return result;
        }
    }
    return NULL;
}

// -------------------------------------------------------------------
// vcDiff  –  show differences between the working file and its last
// committed version.
//
// Usage:
//   vc diff <file>
//
// Extracts the last committed version of the file from the most
// recent commit zip that contains it, then runs the system diff
// Usage:
//   vc diff <file>              terminal diff (default)
//   vc diff --diff <file>       terminal diff (explicit)
//   vc diff --meld <file>       meld GUI diff
// -------------------------------------------------------------------
int vcDiff(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    // Parse flags and filename.
    const char *fileName = NULL;
    bool useMeld = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--meld") == 0) {
            useMeld = true;
        } else if (strcmp(argv[i], "--diff") == 0) {
            useMeld = false;
        } else if (argv[i][0] != '-') {
            fileName = argv[i];
        } else {
            fprintf(stderr, "vcDiff: unknown option '%s'\n"
                            "        Usage: vc diff [--diff|--meld] <file>\n",
                    argv[i]);
            return -1;
        }
    }

    if (fileName == NULL) {
        fprintf(stderr, "vcDiff: no file specified.\n"
                        "        Usage: vc diff [--diff|--meld] <file>\n");
        return -1;
    }

    // Check meld is available if requested.
    if (useMeld && system("which meld > /dev/null 2>&1") != 0) {
        fprintf(stderr, "vcDiff: meld not found. Install it with:\n"
                        "          sudo apt install meld       (Debian/Ubuntu)\n"
                        "          sudo dnf install meld       (Fedora)\n"
                        "          brew install --cask meld    (macOS)\n"
                        "        Falling back to terminal diff.\n\n");
        useMeld = false;
    }

    // Resolve the relative path the same way the index stores it.
    char relPath[MAX_DIR_PATH];
    char fullPath[MAX_DIR_PATH];

    if (fileName[0] == '/')
        snprintf(fullPath, sizeof(fullPath), "%s", fileName);
    else
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, fileName);

    // Build relative path by stripping vcTopDir prefix.
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
            snprintf(relPath, sizeof(relPath), "%s", fileName);
        }
    } else {
        // File doesn't exist on disk.
        fprintf(stderr, "vcDiff: '%s' not found on disk.\n", fileName);
        return -1;
    }

    // Check the file is tracked.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);
    if (vcIndexFind(entries, count, relPath) < 0) {
        fprintf(stderr, "vcDiff: '%s' is not tracked by vc.\n"
                        "        Run 'vc add %s' to start tracking it.\n",
                fileName, fileName);
        vcIndexFree(entries, count);
        return -1;
    }
    vcIndexFree(entries, count);

    // Find the newest commit zip that contains this file.
    char *zipPath = findLatestZipForFile(relPath);
    if (zipPath == NULL) {
        fprintf(stderr, "vcDiff: '%s' has not been committed yet.\n"
                        "        Run 'vc commit' to create a baseline.\n",
                fileName);
        return -1;
    }

    // Extract committed version to a temp file.
    char *tmpPath = extractToTemp(zipPath, relPath);
    free(zipPath);

    if (tmpPath == NULL) {
        fprintf(stderr, "vcDiff: could not extract committed version of '%s'.\n",
                fileName);
        return -1;
    }

    // Run the chosen diff tool.
    char cmd[MAX_DIR_PATH * 2 + 128];
    int  diffExit = 0;

    if (useMeld) {
        // Meld is a GUI tool — launch it and don't wait (it's non-blocking).
        printf("Launching meld: %s\n", relPath);
        snprintf(cmd, sizeof(cmd), "meld '%s' '%s' &", tmpPath, fullPath);
        system(cmd);
        // Can't determine differences from a GUI tool — just clean up later.
        // Give meld a moment to open the file before we delete the temp.
        sleep(2);
    } else {
        // Terminal diff — unified format, capture exit code via sentinel.
        printf("--- %s  (committed)\n", relPath);
        printf("+++ %s  (working copy)\n\n", relPath);
        fflush(stdout);

        snprintf(cmd, sizeof(cmd),
                 "diff -u '%s' '%s'; echo \"DIFF_EXIT:$?\"",
                 tmpPath, fullPath);

        FILE *pipe = popen(cmd, "r");
        if (pipe != NULL) {
            char line[4096];
            int lineNum = 0;
            while (fgets(line, sizeof(line), pipe) != NULL) {
                if (strncmp(line, "DIFF_EXIT:", 10) == 0) {
                    diffExit = atoi(line + 10);
                    continue;
                }
                // Skip --- and +++ lines (show our own header instead).
                lineNum++;
                if (lineNum <= 2) continue;
                printf("%s", line);
            }
            pclose(pipe);
        }

        if (diffExit == 0)
            printf("No differences — working copy matches last commit.\n");
    }

    remove(tmpPath);
    free(tmpPath);

    return (diffExit <= 1) ? 0 : -1;
}
