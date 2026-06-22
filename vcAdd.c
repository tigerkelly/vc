#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "vc.h"

// -------------------------------------------------------------------
// Module-level staging context — declared before helpers so all
// functions in this file can access it.
// -------------------------------------------------------------------
typedef struct {
    IndexEntry *entries;
    int         count;
    int         staged;
    int         binaryDefault;  // 0=ask, 1=stage silently, -1=ignore silently
} AddCtx;

static AddCtx _addCtx;

// -------------------------------------------------------------------
// is_binary_file  –  sniff up to 8 KB of the file for non-text bytes.
// -------------------------------------------------------------------
static bool is_binary_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0) return true;
        if (buf[i] < 8 || (buf[i] > 13 && buf[i] < 32 && buf[i] != 27))
            return true;
    }
    return false;
}

// -------------------------------------------------------------------
// binary_ok cache  –  .vc/binary_ok stores patterns the user has
// already answered "stage anyway" for. Checked before prompting so
// we don't ask again on subsequent vc add runs.
// -------------------------------------------------------------------
#define BINARY_OK_FILE ".vc/binary_ok"

static bool is_binary_ok(const char *pattern) {
    char okPath[MAX_DIR_PATH];
    snprintf(okPath, sizeof(okPath), "%s/%s", vcTopDir, BINARY_OK_FILE);
    FILE *f = fopen(okPath, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[--l] = '\0';
        if (strcmp(line, pattern) == 0) { fclose(f); return true; }
    }
    fclose(f);
    return false;
}

static void save_binary_ok(const char *pattern) {
    char okPath[MAX_DIR_PATH];
    snprintf(okPath, sizeof(okPath), "%s/%s", vcTopDir, BINARY_OK_FILE);
    FILE *f = fopen(okPath, "a");
    if (!f) f = fopen(okPath, "w");
    if (!f) return;
    fprintf(f, "%s\n", pattern);
    fclose(f);
}

// -------------------------------------------------------------------
// is_already_ignored  –  check if a pattern is in .vcignore.
// -------------------------------------------------------------------
static bool is_already_ignored(const char *pattern) {
    char ignorePath[MAX_DIR_PATH];
    snprintf(ignorePath, sizeof(ignorePath), "%s/.vcignore", vcTopDir);
    FILE *f = fopen(ignorePath, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[--l] = '\0';
        if (strcmp(line, pattern) == 0) { fclose(f); return true; }
    }
    fclose(f);
    return false;
}

// -------------------------------------------------------------------
// append_to_vcignore  –  append a pattern to .vcignore.
// -------------------------------------------------------------------
static void append_to_vcignore(const char *pattern) {
    char ignorePath[MAX_DIR_PATH];
    snprintf(ignorePath, sizeof(ignorePath), "%s/.vcignore", vcTopDir);
    FILE *f = fopen(ignorePath, "a");
    if (!f) f = fopen(ignorePath, "w");
    if (!f) {
        fprintf(stderr, "vcAdd: cannot write .vcignore: %s\n",
                strerror(errno));
        return;
    }
    fprintf(f, "%s\n", pattern);
    fclose(f);
    printf("  Added '%s' to .vcignore\n", pattern);
}

// -------------------------------------------------------------------
// prompt_ignore  –  ask whether to add file or extension to .vcignore.
// Called for binary files and files without an extension.
// Returns true if the user chose to ignore (so caller should skip staging).
// -------------------------------------------------------------------
static bool prompt_ignore(const char *path) {
    const char *rel = path;
    size_t topLen = vcTopDir ? strlen(vcTopDir) : 0;
    if (topLen && strncmp(path, vcTopDir, topLen) == 0) {
        rel = path + topLen;
        if (*rel == '/') rel++;
    }
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;

    char byName[512], byExt[64];
    snprintf(byName, sizeof(byName), "%s", base);
    const char *dot = strrchr(base, '.');
    bool hasExt = dot && dot != base;
    if (hasExt)
        snprintf(byExt, sizeof(byExt), "*%s", dot);

    // Check if the user already answered "stage anyway" for this
    // file name or extension pattern in a previous vc add run.
    if (is_binary_ok(byName)) return false;
    if (hasExt && is_binary_ok(byExt)) return false;

    // Honour --binary=stage or --binary=ignore default if set.
    if (_addCtx.binaryDefault == 1) {
        // --binary=stage: silently stage, remember for next time.
        save_binary_ok(byName);
        return false;
    } else if (_addCtx.binaryDefault == -1) {
        // --binary=ignore: silently add to .vcignore.
        if (!is_already_ignored(byName))
            append_to_vcignore(byName);
        return true;
    }

    // No default set — prompt interactively.
    printf("\n  Warning: '%s' is a binary file.\n", rel);
    printf("  Add to .vcignore?\n");
    printf("    1) This file only  (%s)\n", byName);
    if (hasExt)
        printf("    2) All %s files   (%s)\n", dot, byExt);
    printf("    n) No, stage it (don't ask again)\n");
    printf("  Choice [1%s/n]: ", hasExt ? "/2" : "");
    fflush(stdout);

    char ans[8] = "";
    if (fgets(ans, sizeof(ans), stdin) == NULL) return false;

    if (ans[0] == '1') {
        if (!is_already_ignored(byName))
            append_to_vcignore(byName);
        else
            printf("  '%s' is already in .vcignore\n", byName);
        return true;   // skip staging
    } else if (hasExt && ans[0] == '2') {
        if (!is_already_ignored(byExt))
            append_to_vcignore(byExt);
        else
            printf("  '%s' is already in .vcignore\n", byExt);
        return true;   // skip staging
    }

    // 'n' — stage it and remember so we don't ask again.
    save_binary_ok(byName);
    return false;
}

// -------------------------------------------------------------------
// vcAddAction  –  vcWalk callback.
// Called for every file (and directory) under the path being added.
// Stages each regular file into the in-memory index.
// -------------------------------------------------------------------
int vcAddAction(char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "vcAddAction: cannot stat %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    // Skip non-regular files and non-directories (sockets, FIFOs, etc).
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
        return 0;

    // For directories: only stage them if they are empty (no files inside).
    // Non-empty directories are represented by their contents in the index.
    if (S_ISDIR(st.st_mode)) {
        // Check whether this directory has any regular files inside.
        DIR *d = opendir(path);
        if (d == NULL) return 0;
        bool hasFiles = false;
        struct dirent *dp;
        while ((dp = readdir(d)) != NULL) {
            if (strcmp(dp->d_name, ".") == 0 ||
                strcmp(dp->d_name, "..") == 0) continue;
            hasFiles = true;
            break;
        }
        closedir(d);
        if (hasFiles) return 0;   // non-empty — files will carry the dir
        // Fall through to stage the empty directory.
    }

    int prevCount = _addCtx.count;

    // For regular files: check if binary.
    // Only prompt if the file appears to be binary, isn't already ignored,
    // and hasn't been previously answered "stage anyway".
    // Text files without extensions (Makefile, Dockerfile, README, etc.)
    // are staged silently — only binary content triggers the warning.
    if (S_ISREG(st.st_mode)) {
        if (is_binary_file(path) && !is_already_ignored(path)) {
            if (prompt_ignore(path))
                return 0;  // user chose to ignore — skip staging
        }
    }

    _addCtx.entries = vcIndexStage(_addCtx.entries,
                                   &_addCtx.count,
                                   path);

    // Find the relative path for display.
    const char *rel = path;
    size_t topLen = strlen(vcTopDir);
    if (strncmp(path, vcTopDir, topLen) == 0) {
        rel = path + topLen;
        if (*rel == '/') rel++;
    }

    if (_addCtx.count > prevCount) {
        printf("  add  (new)      %s%s\n", rel,
               S_ISDIR(st.st_mode) ? "/" : "");
        _addCtx.staged++;
    } else if (_addCtx.count == prevCount && prevCount >= 0) {
        if (_addCtx.entries != NULL) {
            printf("  add  (updated)  %s%s\n", rel,
                   S_ISDIR(st.st_mode) ? "/" : "");
            _addCtx.staged++;
        } else {
            fprintf(stderr, "  warning: could not stage %s\n", rel);
        }
    }

    return 0;
}

// -------------------------------------------------------------------
// vcAdd  –  entry point called from vc.c.
//
// argc / argv mirror the full program argc/argv so the user can write:
//
//   vc add              – stage everything under the project root
//   vc add src/main.c   – stage one specific file
//   vc add src/         – stage everything under src/
//
// -------------------------------------------------------------------
int vcAdd(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    // 1. Load the current index from disk.
    _addCtx.count         = vcIndexLoad(&_addCtx.entries);
    _addCtx.staged        = 0;
    _addCtx.binaryDefault = 0;  // 0=ask, 1=stage silently, -1=ignore silently

    if (_addCtx.count < 0) {
        fprintf(stderr, "vcAdd: failed to load index.\n");
        return -1;
    }

    // 2. Parse flags and collect paths.
    //    Flags: --binary=stage   silently stage all binary files
    //           --binary=ignore  silently add all binary files to .vcignore
    //    argv[0] = "vc", argv[1] = "add", argv[2..] = flags and/or paths.
    int firstPath = 2;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--binary=", 9) == 0) {
            const char *val = argv[i] + 9;
            if (strcmp(val, "stage") == 0) {
                _addCtx.binaryDefault = 1;
            } else if (strcmp(val, "ignore") == 0) {
                _addCtx.binaryDefault = -1;
            } else {
                fprintf(stderr, "vcAdd: unknown --binary value '%s'.\n"
                                "       Use --binary=stage or --binary=ignore\n",
                        val);
                return -1;
            }
            firstPath = i + 1;
        } else {
            break;  // first non-flag arg — rest are paths
        }
    }

    // 3. Determine what to stage.
    if (argc <= firstPath) {
        // No paths given – stage the entire project tree.
        printf("Staging all files under %s\n", vcTopDir);
        vcWalk(vcTopDir, vcAddAction);
    } else {
        // Stage each path given on the command line.
        for (int i = firstPath; i < argc; i++) {
            struct stat st;
            // Resolve relative paths against cwd (which vc.c already
            // set to vcTopDir, so relative paths work naturally).
            if (stat(argv[i], &st) == -1) {
                fprintf(stderr, "vcAdd: cannot access '%s': %s\n",
                        argv[i], strerror(errno));
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                printf("Staging files under %s\n", argv[i]);
                vcWalk(argv[i], vcAddAction);
            } else {
                // Single file – build its full path and stage it directly.
                char fullPath[MAX_DIR_PATH];
                if (argv[i][0] == '/') {
                    // Already absolute.
                    snprintf(fullPath, sizeof(fullPath), "%s", argv[i]);
                } else {
                    snprintf(fullPath, sizeof(fullPath), "%s/%s",
                             vcTopDir, argv[i]);
                }
                vcAddAction(fullPath);
            }
        }
    }

    // 3. Save the updated index back to disk.
    if (_addCtx.staged > 0) {
        if (vcIndexSave(_addCtx.entries, _addCtx.count) != 0) {
            fprintf(stderr, "vcAdd: failed to save index.\n");
            vcIndexFree(_addCtx.entries, _addCtx.count);
            return -1;
        }
        printf("\n%d file(s) staged for commit.\n", _addCtx.staged);
    } else {
        printf("Nothing to stage.\n");
    }

    vcIndexFree(_addCtx.entries, _addCtx.count);
    return 0;
}
