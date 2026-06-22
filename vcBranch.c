
#include <stdio.h>
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
// Branch layout inside .vc/
//
//   .vc/HEAD                        – active branch name
//   .vc/branches/                   – one subdirectory per branch
//   .vc/branches/main/
//     index                         – staging index for this branch
//     info                          – metadata (created date, author)
//     HEAD_COMMIT                   – filename of last commit zip
//
// Each branch owns its index and its HEAD_COMMIT pointer.
// Switching branches reconciles the working directory:
//   - Files only on the outgoing branch are deleted from disk.
//   - Files on the incoming branch are restored from the zip
//     recorded in that branch's HEAD_COMMIT file.
//   - Untracked files (in neither index) are left untouched.
// -------------------------------------------------------------------

#define VC_HEAD_FILE     ".vc/HEAD"
#define VC_BRANCHES_DIR  ".vc/branches"
#define DEFAULT_BRANCH   "main"
#define MAX_BRANCH_NAME  128

// Forward declaration.
static char *currentBranch(void);

// -------------------------------------------------------------------
// vcBranchCurrentName  –  public wrapper so other modules can read
// the active branch without duplicating the HEAD-reading logic.
// Returns a heap-allocated string the caller must free.
// -------------------------------------------------------------------
char *vcBranchCurrentName(void) {
    return currentBranch();
}

// -------------------------------------------------------------------
// currentBranch  –  read the branch name from .vc/HEAD.
// Returns a heap-allocated string the caller must free, or NULL.
// -------------------------------------------------------------------
static char *currentBranch(void) {
    FILE *f = fopen(VC_HEAD_FILE, "r");
    if (f == NULL)
        return strdup(DEFAULT_BRANCH);   // no HEAD yet — assume main

    char line[MAX_BRANCH_NAME + 2];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return strdup(DEFAULT_BRANCH);
    }
    fclose(f);

    // Strip trailing newline.
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

    return len > 0 ? strdup(line) : strdup(DEFAULT_BRANCH);
}

// -------------------------------------------------------------------
// setCurrentBranch  –  write the branch name to .vc/HEAD.
// -------------------------------------------------------------------
static bool setCurrentBranch(const char *name) {
    FILE *f = fopen(VC_HEAD_FILE, "w");
    if (f == NULL) {
        fprintf(stderr, "vcBranch: cannot write HEAD: %s\n", strerror(errno));
        return false;
    }
    fprintf(f, "%s\n", name);
    fclose(f);
    return true;
}

// -------------------------------------------------------------------
// branchDir  –  fill buf with the path to a branch's directory.
// -------------------------------------------------------------------
static void branchDir(char *buf, size_t sz, const char *name) {
    snprintf(buf, sz, "%s/%s", VC_BRANCHES_DIR, name);
}

// -------------------------------------------------------------------
// branchExists  –  true if a branch directory already exists.
// -------------------------------------------------------------------
static bool branchExists(const char *name) {
    char path[MAX_DIR_PATH];
    branchDir(path, sizeof(path), name);
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

// -------------------------------------------------------------------
// isValidBranchName  –  reject names with path separators or spaces.
// -------------------------------------------------------------------
static bool isValidBranchName(const char *name) {
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '-') return false;   // cannot start with a dash
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ' ' || *p == '\t')
            return false;
    }
    return true;
}

// -------------------------------------------------------------------
// copyFile  –  copy src to dst, returning false on error.
// -------------------------------------------------------------------
static bool copyFile(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (in == NULL) return false;

    FILE *out = fopen(dst, "wb");
    if (out == NULL) { fclose(in); return false; }

    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }

    fclose(in);
    fclose(out);
    return ok;
}

// -------------------------------------------------------------------
// writeBranchInfo  –  write metadata into <branchDir>/info.
// -------------------------------------------------------------------
static void writeBranchInfo(const char *name, const char *fromBranch) {
    char path[MAX_DIR_PATH];
    snprintf(path, sizeof(path), "%s/%s/info", VC_BRANCHES_DIR, name);

    FILE *f = fopen(path, "w");
    if (f == NULL) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm);

    extern Config *config;
    const char *author = (config && config->user[0]) ? config->user : "unknown";

    fprintf(f, "branch  = %s\n", name);
    fprintf(f, "from    = %s\n", fromBranch);
    fprintf(f, "author  = %s\n", author);
    fprintf(f, "created = %s\n", timeBuf);
    fclose(f);
}

// -------------------------------------------------------------------
// cmdList  –  list all branches, marking the active one with '*'.
// -------------------------------------------------------------------
static int cmdList(void) {
    DIR *dir = opendir(VC_BRANCHES_DIR);
    if (dir == NULL) {
        printf("No branches found. Run 'vc init' first.\n");
        return 0;
    }

    char *current = currentBranch();
    printf("Branches (* = active):\n\n");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        // Only list directories.
        char path[MAX_DIR_PATH];
        snprintf(path, sizeof(path), "%s/%s", VC_BRANCHES_DIR, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        bool active = (strcmp(entry->d_name, current) == 0);
        printf("  %s %s\n", active ? "*" : " ", entry->d_name);
    }

    closedir(dir);
    printf("\n");
    free(current);
    return 0;
}

// -------------------------------------------------------------------
// cmdCreate  –  create a new branch from the current state.
// The new branch gets a copy of the current index so it starts with
// the same set of tracked files.
// -------------------------------------------------------------------
static int cmdCreate(const char *name) {
    if (!isValidBranchName(name)) {
        fprintf(stderr, "vcBranch: invalid branch name '%s'.\n"
                        "         Names may not start with '-' or contain spaces or '/' characters.\n", name);
        return -1;
    }

    if (branchExists(name)) {
        fprintf(stderr, "vcBranch: branch '%s' already exists.\n"
                        "         Use 'vc branch %s' to switch to it.\n",
                name, name);
        return -1;
    }

    // Create the branch directory.
    char dir[MAX_DIR_PATH];
    branchDir(dir, sizeof(dir), name);
    if (mkdir(dir, 0755) != 0) {
        fprintf(stderr, "vcBranch: cannot create '%s': %s\n", dir, strerror(errno));
        return -1;
    }

    // Record which branch we branched from.
    char *from = currentBranch();

    // Copy the current branch's index into the new branch directory
    // so it starts with the same set of tracked/committed files.
    char srcIndex[MAX_DIR_PATH];
    char dstIndex[MAX_DIR_PATH];
    snprintf(srcIndex, sizeof(srcIndex), "%s/%s/index", VC_BRANCHES_DIR, from);
    snprintf(dstIndex, sizeof(dstIndex), "%s/%s/index", VC_BRANCHES_DIR, name);
    copyFile(srcIndex, dstIndex);   // ignore error – new branch may have empty index

    // Write metadata.
    writeBranchInfo(name, from);

    printf("Branch '%s' created from '%s'.\n", name, from);

    free(from);
    return 0;
}

// -------------------------------------------------------------------
// vcBranchRecordCommit  –  called by vcCommit after a successful
// commit.  Records the zip filename in the active branch's directory
// so that cmdSwitch knows exactly which archive to restore from.
//
// Writes one line to  .vc/branches/<branch>/HEAD_COMMIT
// -------------------------------------------------------------------
int vcBranchRecordCommit(const char *zipName) {
    char *branch = currentBranch();
    if (branch == NULL) return -1;

    char path[MAX_DIR_PATH];
    snprintf(path, sizeof(path), "%s/%s/HEAD_COMMIT",
             VC_BRANCHES_DIR, branch);
    free(branch);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "vcBranchRecordCommit: cannot write '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", zipName);
    fclose(f);
    return 0;
}

// -------------------------------------------------------------------
// branchCommitZip  –  return the path to the last commit zip for a
// named branch by reading its HEAD_COMMIT file.
// Falls back to the newest zip in .vc/data/ if no HEAD_COMMIT exists
// (e.g. the branch has never been committed to directly).
// Returns false if no zip can be found at all.
// -------------------------------------------------------------------
static bool branchCommitZip(const char *branchName, char *buf, size_t sz) {
    char headCommit[MAX_DIR_PATH];
    snprintf(headCommit, sizeof(headCommit), "%s/%s/HEAD_COMMIT",
             VC_BRANCHES_DIR, branchName);

    FILE *f = fopen(headCommit, "r");
    if (f != NULL) {
        char zipName[MAX_DIR_PATH];
        if (fgets(zipName, sizeof(zipName), f) != NULL) {
            fclose(f);
            // Strip trailing newline.
            size_t len = strlen(zipName);
            if (len > 0 && zipName[len-1] == '\n') zipName[--len] = '\0';

            snprintf(buf, sz, ".vc/data/%s", zipName);

            // Verify the file actually exists before returning it.
            struct stat st;
            if (stat(buf, &st) == 0) return true;
        } else {
            fclose(f);
        }
    }

    // No HEAD_COMMIT or the recorded zip is missing — fall back to
    // the newest zip in .vc/data/ as a best-effort recovery.
    DIR *dir = opendir(".vc/data");
    if (dir == NULL) return false;

    char best[MAX_DIR_PATH] = "";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".zip") != 0)
            continue;
        if (strcmp(entry->d_name, best) > 0)
            snprintf(best, sizeof(best), "%s", entry->d_name);
    }
    closedir(dir);

    if (best[0] == '\0') return false;
    snprintf(buf, sz, ".vc/data/%s", best);
    return true;
}

// Forward declaration — defined later in this file.
static bool restoreFileFromZip(const char *zipPath, const char *relPath);

// -------------------------------------------------------------------
// vcRestoreFileFromZip  –  public wrapper so vcCheckout can extract
// individual files from a commit archive without duplicating the code.
// -------------------------------------------------------------------
bool vcRestoreFileFromZip(const char *zipPath, const char *relPath) {
    return restoreFileFromZip(zipPath, relPath);
}

// -------------------------------------------------------------------
// vcBranchCommitZip  –  public wrapper so vcCheckout can find the
// latest commit zip for the active branch.
// -------------------------------------------------------------------
bool vcBranchCommitZip(char *buf, size_t sz) {
    char *branch = currentBranch();
    bool ok = branchCommitZip(branch, buf, sz);
    free(branch);
    return ok;
}

// -------------------------------------------------------------------
// restoreFileFromZip  –  extract a single file (relPath) from the zip
// archive at zipPath, writing it to vcTopDir/relPath.
// Creates parent directories as needed.
// Returns true on success.
// -------------------------------------------------------------------
static bool restoreFileFromZip(const char *zipPath,
                                const char *relPath) {
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) {
        fprintf(stderr, "vcBranch: cannot open archive '%s'\n", zipPath);
        return false;
    }

    zip_file_t *zf = zip_fopen(za, relPath, 0);
    if (zf == NULL) {
        // File not in this archive – that's allowed (may be new on this branch).
        zip_close(za);
        return false;
    }

    // Build the destination path.
    char destPath[MAX_DIR_PATH];
    snprintf(destPath, sizeof(destPath), "%s/%s", vcTopDir, relPath);

    // Ensure parent directories exist.
    char parent[MAX_DIR_PATH];
    snprintf(parent, sizeof(parent), "%s", destPath);
    char *slash = strrchr(parent, '/');
    if (slash != NULL) {
        *slash = '\0';
        // mkdir -p equivalent: walk from vcTopDir down.
        char *p = parent + strlen(vcTopDir) + 1;
        while ((p = strchr(p, '/')) != NULL) {
            *p = '\0';
            mkdir(parent, 0755);   // ignore error if already exists
            *p = '/';
            p++;
        }
        mkdir(parent, 0755);
    }

    FILE *out = fopen(destPath, "wb");
    if (out == NULL) {
        fprintf(stderr, "vcBranch: cannot write '%s': %s\n",
                destPath, strerror(errno));
        zip_fclose(zf);
        zip_close(za);
        return false;
    }

    char buf[8192];
    zip_int64_t n;
    while ((n = zip_fread(zf, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, out);

    fclose(out);
    zip_fclose(zf);
    zip_close(za);
    return true;
}

// -------------------------------------------------------------------
// loadBranchIndex  –  load the index for a named branch directly
// from its file, without going through vcIndexLoad (which reads
// whatever HEAD currently points to).
// -------------------------------------------------------------------
static int loadBranchIndex(const char *branchName,
                            IndexEntry **entriesOut) {
    *entriesOut = NULL;

    char indexFile[MAX_DIR_PATH];
    snprintf(indexFile, sizeof(indexFile),
             "%s/%s/index", VC_BRANCHES_DIR, branchName);

    FILE *f = fopen(indexFile, "r");
    if (f == NULL) return 0;   // empty / nonexistent = no entries

    // Count non-blank lines.
    char line[4096];
    int count = 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (line[0] != '\0' && line[0] != '\n') count++;
    rewind(f);

    IndexEntry *entries = calloc((size_t)count, sizeof(IndexEntry));
    if (entries == NULL) { fclose(f); return -1; }

    int idx = 0;
    while (fgets(line, sizeof(line), f) != NULL && idx < count) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        char *path  = strtok(line, "|");
        char *state = strtok(NULL, "|");
        char *mtime = strtok(NULL, "|");
        char *sz    = strtok(NULL, "|");

        if (!path || !state || !mtime) continue;

        entries[idx].path  = strdup(path);
        entries[idx].mtime = (time_t)atol(mtime);
        entries[idx].size  = sz ? (off_t)atol(sz) : 0;

        if (strcmp(state, "staged") == 0)
            entries[idx].state = INDEX_STAGED;
        else if (strcmp(state, "modified") == 0)
            entries[idx].state = INDEX_MODIFIED;
        else
            entries[idx].state = INDEX_COMMITTED;

        idx++;
    }

    fclose(f);
    *entriesOut = entries;
    return idx;
}

// -------------------------------------------------------------------
// cmdSwitch  –  switch to an existing branch.
//
// Working-directory reconciliation:
//
//   File only in outgoing index  → delete from disk
//   File only in incoming index  → restore from incoming branch's
//                                   latest commit zip
//   File in both indexes         → restore incoming version so disk
//                                   matches the incoming branch's
//                                   last committed state
//   Untracked file (neither)     → leave untouched
// -------------------------------------------------------------------
static int cmdSwitch(const char *name) {
    if (!branchExists(name)) {
        fprintf(stderr, "vcBranch: branch '%s' does not exist.\n"
                        "         Use 'vc branch %s' to create it.\n", name, name);
        return -1;
    }

    // Refuse to switch if the user started in a subdirectory.
    // Switching branches can delete the current directory from disk,
    // which leaves the shell in a non-existent path.
    // vcStartDir is the cwd captured before vc.c called chdir(vcTopDir).
    if (vcStartDir == NULL || strcmp(vcStartDir, vcTopDir) != 0) {
        fprintf(stderr, "vcBranch: you must be in the project root to switch branches.\n"
                        "         Run:  cd %s\n", vcTopDir);
        return -1;
    }

    char *current = currentBranch();

    if (strcmp(current, name) == 0) {
        printf("Already on branch '%s'.\n", name);
        free(current);
        return 0;
    }

    // Warn about staged but uncommitted files — they stay on this branch
    // and will be waiting when the user switches back.
    IndexEntry *outEntries = NULL;
    int outCount = loadBranchIndex(current, &outEntries);
    int staged = 0;
    for (int i = 0; i < outCount; i++)
        if (outEntries[i].state == INDEX_STAGED) staged++;

    if (staged > 0) {
        fprintf(stderr, "vcBranch: %d file(s) are staged but not committed.\n"
                        "         They will remain staged on branch '%s' and\n"
                        "         will be waiting when you switch back.\n"
                        "         Switch anyway? [y/N]: ", staged, current);
        fflush(stderr);
        char answer[8] = {0};
        if (fgets(answer, sizeof(answer), stdin) == NULL ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("Switch cancelled.\n");
            vcIndexFree(outEntries, outCount);
            free(current);
            return 0;
        }
    }

    // Load the incoming branch's index.
    IndexEntry *inEntries = NULL;
    int inCount = loadBranchIndex(name, &inEntries);

    // Find the last commit zip for the incoming branch specifically.
    char zipPath[MAX_DIR_PATH];
    bool hasZip = branchCommitZip(name, zipPath, sizeof(zipPath));

    int deleted = 0, restored = 0, skipped = 0;

    // --- Step 1: remove files/dirs that exist only on the outgoing branch ---
    // Process files first, then directories (so dirs are empty before rmdir).
    // Two passes: files, then directories (identified by trailing '/').

    // Pass 1a: remove files only.
    for (int i = 0; i < outCount; i++) {
        const char *relPath = outEntries[i].path;
        size_t rlen = strlen(relPath);
        bool isDir = (rlen > 0 && relPath[rlen-1] == '/');
        if (isDir) continue;   // handle in pass 1b

        bool inIncoming = false;
        for (int j = 0; j < inCount; j++) {
            if (strcmp(inEntries[j].path, relPath) == 0) {
                inIncoming = true;
                break;
            }
        }
        if (!inIncoming) {
            char fullPath[MAX_DIR_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, relPath);
            if (remove(fullPath) == 0) {
                printf("  removed   %s\n", relPath);
                deleted++;
            }
        }
    }

    // Pass 1b: remove directories only (now empty after pass 1a).
    for (int i = 0; i < outCount; i++) {
        const char *relPath = outEntries[i].path;
        size_t rlen = strlen(relPath);
        bool isDir = (rlen > 0 && relPath[rlen-1] == '/');
        if (!isDir) continue;

        bool inIncoming = false;
        for (int j = 0; j < inCount; j++) {
            if (strcmp(inEntries[j].path, relPath) == 0) {
                inIncoming = true;
                break;
            }
        }
        if (!inIncoming) {
            // Build path without trailing slash for rmdir.
            char fullPath[MAX_DIR_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, relPath);
            size_t flen = strlen(fullPath);
            if (flen > 0 && fullPath[flen-1] == '/')
                fullPath[flen-1] = '\0';

            if (rmdir(fullPath) == 0) {
                printf("  removed   %s\n", relPath);
                deleted++;
            } else {
                // Directory may still have untracked files — warn but continue.
                fprintf(stderr, "  warning: could not remove %s: %s\n",
                        relPath, strerror(errno));
            }
        }
    }

    // --- Step 2: restore files/dirs from the incoming branch ---
    for (int i = 0; i < inCount; i++) {
        const char *relPath = inEntries[i].path;
        size_t rlen = strlen(relPath);
        bool isDir = (rlen > 0 && relPath[rlen-1] == '/');

        if (isDir) {
            // Create the directory on disk.
            char fullPath[MAX_DIR_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, relPath);
            // Strip trailing slash for mkdir.
            size_t flen = strlen(fullPath);
            if (flen > 0 && fullPath[flen-1] == '/')
                fullPath[flen-1] = '\0';

            struct stat st;
            if (stat(fullPath, &st) == 0) {
                // Already exists — nothing to do.
                inEntries[i].state = INDEX_COMMITTED;
            } else if (mkdir(fullPath, 0755) == 0) {
                printf("  restored  %s\n", relPath);
                inEntries[i].state = INDEX_COMMITTED;
                restored++;
            } else {
                fprintf(stderr, "  warning: could not create %s: %s\n",
                        relPath, strerror(errno));
            }
            continue;
        }

        if (hasZip) {
            if (restoreFileFromZip(zipPath, relPath)) {
                char fullPath[MAX_DIR_PATH];
                snprintf(fullPath, sizeof(fullPath), "%s/%s",
                         vcTopDir, relPath);
                struct stat st;
                if (stat(fullPath, &st) == 0) {
                    inEntries[i].mtime = st.st_mtime;
                    inEntries[i].size  = st.st_size;
                }
                inEntries[i].state = INDEX_COMMITTED;
                printf("  restored  %s\n", relPath);
                restored++;
            } else {
                printf("  skipped   %s  (not in archive)\n", relPath);
                skipped++;
            }
        } else {
            printf("  skipped   %s  (no commit archive found)\n", relPath);
            skipped++;
        }
    }

    // --- Step 3: update HEAD ---
    if (!setCurrentBranch(name)) {
        vcIndexFree(outEntries, outCount);
        vcIndexFree(inEntries, inCount);
        free(current);
        return -1;
    }

    // --- Step 4: save the refreshed incoming index so vcStatus sees
    //             correct mtimes for the just-restored files ---
    // Write directly to the incoming branch's index file.
    char indexFile[MAX_DIR_PATH];
    snprintf(indexFile, sizeof(indexFile),
             "%s/%s/index", VC_BRANCHES_DIR, name);
    char tmpFile[MAX_DIR_PATH];
    snprintf(tmpFile, sizeof(tmpFile), "%s.tmp", indexFile);

    FILE *idxf = fopen(tmpFile, "w");
    if (idxf != NULL) {
        for (int i = 0; i < inCount; i++) {
            const char *stateStr;
            switch (inEntries[i].state) {
                case INDEX_STAGED:   stateStr = "staged";   break;
                case INDEX_MODIFIED: stateStr = "modified"; break;
                default:             stateStr = "committed"; break;
            }
            fprintf(idxf, "%s|%s|%ld|%ld\n",
                    inEntries[i].path, stateStr,
                    (long)inEntries[i].mtime,
                    (long)inEntries[i].size);
        }
        fclose(idxf);
        rename(tmpFile, indexFile);
    }

    printf("\nSwitched from '%s' to branch '%s'.\n", current, name);
    printf("  %d file(s) removed, %d restored, %d skipped.\n",
           deleted, restored, skipped);

    vcIndexFree(outEntries, outCount);
    vcIndexFree(inEntries, inCount);
    free(current);
    return 0;
}

// -------------------------------------------------------------------
// cmdDelete  –  delete a branch (cannot delete the active branch).
// -------------------------------------------------------------------
static int cmdDelete(const char *name) {
    char *current = currentBranch();

    if (strcmp(current, name) == 0) {
        fprintf(stderr, "vcBranch: cannot delete the active branch '%s'.\n"
                        "         Switch to another branch first.\n", name);
        free(current);
        return -1;
    }
    free(current);

    if (!branchExists(name)) {
        fprintf(stderr, "vcBranch: branch '%s' does not exist.\n", name);
        return -1;
    }

    // Remove the branch directory and its contents.
    char dir[MAX_DIR_PATH];
    branchDir(dir, sizeof(dir), name);

    // Delete known files inside the branch directory.
    const char *files[] = { "index", "info", NULL };
    for (int i = 0; files[i] != NULL; i++) {
        char path[MAX_DIR_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        remove(path);   // ignore error if file doesn't exist
    }

    if (rmdir(dir) != 0) {
        fprintf(stderr, "vcBranch: could not remove '%s': %s\n",
                dir, strerror(errno));
        fprintf(stderr, "          The directory may contain unexpected files.\n");
        return -1;
    }

    printf("Branch '%s' deleted.\n", name);
    return 0;
}

// -------------------------------------------------------------------
// vcBranchEnsureInit  –  called by vcInit to create the branches/
// directory and the default 'main' branch.
// -------------------------------------------------------------------
int vcBranchEnsureInit(void) {
    // Create .vc/branches/ if it doesn't exist.
    struct stat st;
    if (stat(VC_BRANCHES_DIR, &st) != 0) {
        if (mkdir(VC_BRANCHES_DIR, 0755) != 0) {
            fprintf(stderr, "vcBranch: cannot create '%s': %s\n",
                    VC_BRANCHES_DIR, strerror(errno));
            return -1;
        }
    }

    // Create the default 'main' branch directory.
    char mainDir[MAX_DIR_PATH];
    branchDir(mainDir, sizeof(mainDir), DEFAULT_BRANCH);
    if (stat(mainDir, &st) != 0) {
        if (mkdir(mainDir, 0755) != 0) {
            fprintf(stderr, "vcBranch: cannot create '%s': %s\n",
                    mainDir, strerror(errno));
            return -1;
        }
        writeBranchInfo(DEFAULT_BRANCH, "(root)");

        // Create an empty index file so it exists from the start.
        char indexFile[MAX_DIR_PATH];
        snprintf(indexFile, sizeof(indexFile), "%s/index", mainDir);
        FILE *f = fopen(indexFile, "w");
        if (f) fclose(f);
    }

    // Write HEAD pointing to main.
    if (stat(VC_HEAD_FILE, &st) != 0)
        setCurrentBranch(DEFAULT_BRANCH);

    return 0;
}

// -------------------------------------------------------------------
// vcBranch  –  entry point called from vc.c.
//
// Usage:
//   vc branch                   List all branches, showing the active one
//   vc branch <name>            Switch to <name> if it exists, otherwise
//                               create it and switch to it immediately
//   vc branch --delete <name>   Delete a branch
// -------------------------------------------------------------------
int vcBranch(char *branchName) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // No argument – list branches.
    if (branchName == NULL || branchName[0] == '\0')
        return cmdList();

    // --delete=<name> (built by dispatcher from "vc branch --delete <name>")
    if (strncmp(branchName, "--delete=", 9) == 0)
        return cmdDelete(branchName + 9);

    // Bare --delete with no name (shouldn't reach here but guard anyway).
    if (strcmp(branchName, "--delete") == 0) {
        fprintf(stderr, "vcBranch: --delete requires a branch name.\n"
                        "         Usage: vc branch --delete <name>\n");
        return -1;
    }

    // Any other name: switch if it exists, create-then-switch if it doesn't.
    if (branchExists(branchName)) {
        return cmdSwitch(branchName);
    } else {
        // Create the branch then switch to it in one step.
        if (cmdCreate(branchName) != 0)
            return -1;
        return cmdSwitch(branchName);
    }
}
