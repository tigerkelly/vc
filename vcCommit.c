#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <zip.h>

#include "vc.h"

// Forward declaration.
static void vcCommitAddEmptyDirs(const char *dirPath,
                                  IndexEntry *entries, int count);

// Max files we'll collect in one commit.
#define MAX_COMMIT_FILES 4096

// Holds the zip handle and a running count while vcWalk visits each file.
typedef struct {
    zip_t  *za;
    int     count;
    char    zipPath[MAX_DIR_PATH];
} CommitCtx;

// Module-level context so the walk callback can reach it without a global arg.
static CommitCtx _ctx;

// ---------------------------------------------------------------------
// vcCommitAction  –  called by vcWalk for every regular file.
// Adds the file to the open zip archive, preserving its path relative
// to vcTopDir so the tree can be restored exactly.
// ---------------------------------------------------------------------
int vcCommitAction(char *path) {

    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "vcCommitAction: cannot stat %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    // Skip non-regular files — but handle directory entries from the index
    // which are stored with a trailing '/' on their path.
    if (!S_ISREG(st.st_mode)) {
        if (S_ISDIR(st.st_mode)) {
            // Directory entry from the index — add as zip dir entry.
            const char *entryName = path;
            size_t topLen = strlen(vcTopDir);
            if (strncmp(path, vcTopDir, topLen) == 0) {
                entryName = path + topLen;
                if (*entryName == '/') entryName++;
            }
            // Strip trailing slash from path before building entry name.
            char dirEntry[MAX_DIR_PATH];
            size_t elen = strlen(entryName);
            if (elen > 0 && entryName[elen-1] == '/')
                snprintf(dirEntry, sizeof(dirEntry), "%s", entryName);
            else
                snprintf(dirEntry, sizeof(dirEntry), "%s/", entryName);

            zip_int64_t idx = zip_dir_add(_ctx.za, dirEntry,
                                           ZIP_FL_ENC_UTF_8);
            if (idx >= 0) {
                printf("  adding: %s\n", dirEntry);
            }
        }
        return 0;
    }

    // Build the in-zip entry name relative to the project root so that
    // extracting the zip recreates the original directory structure.
    // e.g.  /home/user/myproject/src/main.c  →  src/main.c
    const char *entryName = path;
    size_t topLen = strlen(vcTopDir);
    if (strncmp(path, vcTopDir, topLen) == 0) {
        entryName = path + topLen;
        if (*entryName == '/')
            entryName++;           // strip leading slash
    }

    // Add the file to the zip using a source backed by the file on disk.
    zip_source_t *src = zip_source_file(_ctx.za, path, 0, 0);
    if (src == NULL) {
        fprintf(stderr, "vcCommitAction: zip_source_file failed for %s: %s\n",
                path, zip_error_strerror(zip_get_error(_ctx.za)));
        return -1;
    }

    zip_int64_t idx = zip_file_add(_ctx.za, entryName, src,
                                   ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (idx < 0) {
        fprintf(stderr, "vcCommitAction: zip_file_add failed for %s: %s\n",
                path, zip_error_strerror(zip_get_error(_ctx.za)));
        zip_source_free(src);
        return -1;
    }

    _ctx.count++;
    printf("  adding: %s\n", entryName);

    return 0;
}

// ---------------------------------------------------------------------
// buildManifest  –  writes a small text block into the zip as
// "MANIFEST.txt" so every archive is self-describing.
// ---------------------------------------------------------------------
static int buildManifest(zip_t *za, const char *message,
                          const char *zipName, int fileCount) {

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S UTC", tm);

    // Pull author from the global config loaded in vc.c.
    extern Config *config;
    const char *author = (config && config->user[0]) ? config->user : "unknown";
    const char *project = (config && config->project[0]) ? config->project : "unknown";

    char manifest[2048];
    int n = snprintf(manifest, sizeof(manifest),
        "vc commit manifest\n"
        "------------------\n"
        "archive : %s\n"
        "project : %s\n"
        "author  : %s\n"
        "date    : %s\n"
        "files   : %d\n"
        "message : %s\n",
        zipName, project, author, timeBuf, fileCount, message);

    if (n < 0 || (size_t)n >= sizeof(manifest)) {
        fprintf(stderr, "buildManifest: manifest buffer overflow\n");
        return -1;
    }

    zip_source_t *src = zip_source_buffer(za, strdup(manifest),
                                          (zip_uint64_t)n, 1);
    if (src == NULL) {
        fprintf(stderr, "buildManifest: zip_source_buffer failed\n");
        return -1;
    }

    zip_int64_t idx = zip_file_add(za, "MANIFEST.txt", src,
                                   ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (idx < 0) {
        fprintf(stderr, "buildManifest: zip_file_add failed: %s\n",
                zip_error_strerror(zip_get_error(za)));
        zip_source_free(src);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------
// makeZipPath  –  fills buf with  .vc/data/YYYYMMDD_HHMMSS.zip
// If a zip with that name already exists (two commits in the same second),
// appends _2, _3 etc. until a unique name is found.
// ---------------------------------------------------------------------
static void makeZipPath(char *buf, size_t bufSize) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char timePart[32];
    strftime(timePart, sizeof(timePart), "%Y%m%d_%H%M%S", tm);

    snprintf(buf, bufSize, "%s/.vc/%s/%s.zip",
             vcTopDir, VC_DATA_DIR, timePart);

    // If the file already exists, append _2, _3, etc.
    if (access(buf, F_OK) == 0) {
        for (int n = 2; n < 1000; n++) {
            snprintf(buf, bufSize, "%s/.vc/%s/%s_%d.zip",
                     vcTopDir, VC_DATA_DIR, timePart, n);
            if (access(buf, F_OK) != 0)
                break;
        }
    }
}

// ---------------------------------------------------------------------
// vcCommitAddEmptyDirs  –  walk dirPath and add a zip directory entry
// for every directory that has no indexed files inside it.
// Also recurses to catch nested empty dirs inside non-empty ones.
// ---------------------------------------------------------------------
static void vcCommitAddEmptyDirs(const char *dirPath,
                                  IndexEntry *entries, int count) {
    DIR *dir = opendir(dirPath);
    if (dir == NULL) return;

    struct dirent *dp;
    struct stat st;
    char path[MAX_DIR_PATH];

    while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0  ||
            strcmp(dp->d_name, "..") == 0 ||
            strcmp(dp->d_name, VC_DIR) == 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dirPath, dp->d_name);
        if (stat(path, &st) == -1 || !S_ISDIR(st.st_mode)) continue;

        // Build the relative path for the zip entry.
        const char *rel = path;
        size_t topLen = strlen(vcTopDir);
        if (strncmp(path, vcTopDir, topLen) == 0) {
            rel = path + topLen;
            if (*rel == '/') rel++;
        }

        // Check whether this directory contains any indexed file.
        size_t relLen = strlen(rel);
        bool hasFile = false;
        for (int i = 0; i < count; i++) {
            if (strncmp(entries[i].path, rel, relLen) == 0 &&
                (entries[i].path[relLen] == '/' ||
                 entries[i].path[relLen] == '\0')) {
                hasFile = true;
                break;
            }
        }

        if (!hasFile) {
            // Truly empty (or all-untracked) — add explicit zip dir entry.
            char dirEntry[MAX_DIR_PATH];
            snprintf(dirEntry, sizeof(dirEntry), "%s/", rel);
            zip_int64_t idx = zip_dir_add(_ctx.za, dirEntry,
                                           ZIP_FL_ENC_UTF_8);
            if (idx >= 0)
                printf("  adding: %s\n", dirEntry);
        }

        // Always recurse — there may be nested empty dirs inside.
        vcCommitAddEmptyDirs(path, entries, count);
    }
    closedir(dir);
}

// ---------------------------------------------------------------------
// vcCommit  –  entry point called from vc.c
// Only commits files that are staged in .vc/index.
//
// Usage:
//   vc commit                        – prompts for a message interactively
//   vc commit --msg "my message"     – flag form (preferred)
//   vc commit --msg=my message       – equals form
//   vc commit "my message"           – bare quoted text (still supported)
// ---------------------------------------------------------------------
int vcCommit(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // 1. Load the index.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);
    if (count < 0) {
        fprintf(stderr, "vcCommit: failed to load index.\n");
        return -1;
    }

    // 2. Count staged files – abort early if nothing is staged.
    int stagedCount = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].state == INDEX_STAGED)
            stagedCount++;
    }

    if (stagedCount == 0) {
        printf("Nothing to commit. Use 'vc add' to stage files first.\n");
        vcIndexFree(entries, count);
        return 0;
    }

    // 3. Get the commit message – from the command line or interactively.
    //    Priority: --msg=<val>  >  --msg <val>  >  bare argv[2]  >  prompt.
    char *message = NULL;
    bool  mustFree = false;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--msg=", 6) == 0) {
            // --msg=the message text
            message = argv[i] + 6;
            break;
        } else if (strcmp(argv[i], "--msg") == 0) {
            // --msg <the message text>
            if (i + 1 < argc) {
                message = argv[++i];
            } else {
                fprintf(stderr, "vcCommit: --msg requires a value.\n");
                vcIndexFree(entries, count);
                return -1;
            }
            break;
        } else if (argv[i][0] != '-') {
            // Bare quoted text: vc commit "my message" (legacy form)
            message = argv[i];
            break;
        }
    }

    if (message == NULL || message[0] == '\0') {
        // Nothing on the command line — prompt interactively.
        // Keep asking until user provides a non-empty message.
        for (;;) {
            message = readline("Commit message: ");
            mustFree = true;
            if (message == NULL) {
                // EOF / Ctrl-D
                fprintf(stderr, "Commit aborted.\n");
                vcIndexFree(entries, count);
                return -1;
            }
            // Strip leading/trailing whitespace for the blank check.
            char *p = message;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '\0') break;
            // Empty or whitespace-only — tell user and try again.
            free(message);
            message  = NULL;
            mustFree = false;
            fprintf(stderr, "  Commit message cannot be empty. "
                            "Please enter a description.\n");
        }
    }

    // 4. Build the destination zip path.
    makeZipPath(_ctx.zipPath, sizeof(_ctx.zipPath));

    // 5. Open (create) the zip archive.
    int zipErr = 0;
    _ctx.za = zip_open(_ctx.zipPath, ZIP_CREATE | ZIP_EXCL, &zipErr);
    if (_ctx.za == NULL) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, zipErr);
        fprintf(stderr, "vcCommit: cannot create %s: %s\n",
                _ctx.zipPath, zip_error_strerror(&ze));
        zip_error_fini(&ze);
        if (mustFree) free(message);
        vcIndexFree(entries, count);
        return -1;
    }

    _ctx.count = 0;

    // 6. Add only staged files to the zip.
    printf("Committing %d staged file(s)...\n", stagedCount);
    for (int i = 0; i < count; i++) {
        if (entries[i].state != INDEX_STAGED)
            continue;

        // Build the full path from the relative index path.
        char fullPath[MAX_DIR_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s/%s",
                 vcTopDir, entries[i].path);

        vcCommitAction(fullPath);
    }

    // 6b. Walk the project tree to pick up empty directories.
    // Files in the index carry their parent directories implicitly, but
    // an empty directory has no files so it would be silently dropped.
    // We scan every directory under vcTopDir and add a zip entry for any
    // that are completely absent from the index (no file inside them).
    vcCommitAddEmptyDirs(vcTopDir, entries, count);

    // 7. Embed the manifest.
    const char *zipName = strrchr(_ctx.zipPath, '/');
    zipName = zipName ? zipName + 1 : _ctx.zipPath;

    if (buildManifest(_ctx.za, message, zipName, _ctx.count) != 0) {
        fprintf(stderr, "vcCommit: warning – manifest could not be written.\n");
    }

    // 8. Finalise the zip.
    if (zip_close(_ctx.za) != 0) {
        fprintf(stderr, "vcCommit: zip_close failed: %s\n",
                zip_error_strerror(zip_get_error(_ctx.za)));
        zip_discard(_ctx.za);
        if (mustFree) free(message);
        vcIndexFree(entries, count);
        return -1;
    }

    // 9. Mark staged entries as committed and save the index.
    vcIndexMarkCommitted(entries, count);
    if (vcIndexSave(entries, count) != 0) {
        fprintf(stderr, "vcCommit: warning – index could not be updated.\n");
    }

    // 10. Record this zip as the branch's latest commit so that
    //     'vc branch --switch' restores from the correct archive.
    vcBranchRecordCommit(zipName);

    // 11. Log and report success.
    vcLog("commit: %s  files=%d  msg=%s\n", zipName, _ctx.count, message);
    printf("Committed  : %d file(s)\n", _ctx.count);
    printf("Archive    : %s\n", _ctx.zipPath);
    printf("Message    : %s\n", message);

    if (mustFree) free(message);
    vcIndexFree(entries, count);
    return 0;
}
