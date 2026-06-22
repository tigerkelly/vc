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
#include "vcDiff3.h"

#include "vc.h"

// -------------------------------------------------------------------
// vcMerge  –  merge another branch into the current branch.
//
// Strategy: "ours wins on conflict"
//   For each file tracked on the source branch:
//     - If not in current branch at all → copy it in (staged)
//     - If in both branches and identical → nothing to do
//     - If in both branches and different → take source version,
//       back up current version as <file>.conflict, mark staged
//
// Usage:
//   vc merge <branch>           Merge named branch into current
//   vc merge <branch> --theirs  On conflict prefer source branch
//   vc merge <branch> --ours    On conflict keep current (default)
// -------------------------------------------------------------------

// Read a file from disk into a heap buffer. Returns NULL on failure.
static char *readFileContents(const char *path, size_t *outSize) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); *outSize = 0; return strdup(""); }
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    *outSize = fread(buf, 1, (size_t)sz, f);
    buf[*outSize] = '\0';
    fclose(f);
    return buf;
}

// Write buf to path, creating parent directories as needed.
static bool writeFileContents(const char *path, const char *buf, size_t sz) {
    // Ensure parent dir.
    char parent[MAX_DIR_PATH];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        char cmd[MAX_DIR_PATH + 16];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", parent);
        system(cmd);
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    fwrite(buf, 1, sz, f);
    fclose(f);
    return true;
}

// Extract a file from the most recent zip of a branch.
// Returns heap buffer, caller must free. NULL if not found.
static char *extractFromBranch(const char *branchName,
                                const char *relPath,
                                size_t *outSize) {
    char headCommit[MAX_DIR_PATH];
    snprintf(headCommit, sizeof(headCommit),
             "%s/.vc/branches/%s/HEAD_COMMIT", vcTopDir, branchName);
    FILE *hf = fopen(headCommit, "r");
    if (hf == NULL) return NULL;
    char zipName[64] = "";
    if (fgets(zipName, sizeof(zipName), hf)) {
        size_t l = strlen(zipName);
        if (l > 0 && zipName[l-1] == '\n') zipName[--l] = '\0';
    }
    fclose(hf);
    if (zipName[0] == '\0') return NULL;

    char zipPath[MAX_DIR_PATH];
    snprintf(zipPath, sizeof(zipPath), "%s/.vc/data/%s", vcTopDir, zipName);

    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) return NULL;

    zip_int64_t idx = zip_name_locate(za, relPath, 0);
    if (idx < 0) { zip_close(za); return NULL; }

    zip_stat_t zst;
    zip_stat_index(za, idx, 0, &zst);
    char *buf = malloc(zst.size + 1);
    if (buf == NULL) { zip_close(za); return NULL; }

    zip_file_t *zf = zip_fopen_index(za, idx, 0);
    *outSize = (size_t)zip_fread(zf, buf, zst.size);
    buf[*outSize] = '\0';
    zip_fclose(zf);
    zip_close(za);
    return buf;
}

int vcMerge(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    if (argc < 3) {
        fprintf(stderr, "vcMerge: usage: vc merge <branch> [--ours|--theirs]\n");
        return -1;
    }

    const char *srcBranch = NULL;
    bool preferTheirs = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--theirs") == 0)      preferTheirs = true;
        else if (strcmp(argv[i], "--ours") == 0)   preferTheirs = false;
        else                                        srcBranch = argv[i];
    }

    if (srcBranch == NULL) {
        fprintf(stderr, "vcMerge: no source branch specified.\n");
        return -1;
    }

    char *currentBranch = vcBranchCurrentName();

    if (strcmp(srcBranch, currentBranch) == 0) {
        fprintf(stderr, "vcMerge: cannot merge a branch into itself.\n");
        free(currentBranch);
        return -1;
    }

    // Verify source branch exists.
    char srcDir[MAX_DIR_PATH];
    snprintf(srcDir, sizeof(srcDir), "%s/.vc/branches/%s", vcTopDir, srcBranch);
    struct stat st;
    if (stat(srcDir, &st) != 0) {
        fprintf(stderr, "vcMerge: branch '%s' does not exist.\n", srcBranch);
        free(currentBranch);
        return -1;
    }

    // Check for uncommitted staged files on current branch.
    IndexEntry *curEntries = NULL;
    int curCount = vcIndexLoad(&curEntries);
    int staged = 0;
    for (int i = 0; i < curCount; i++)
        if (curEntries[i].state == INDEX_STAGED) staged++;
    if (staged > 0) {
        fprintf(stderr, "vcMerge: %d staged file(s) on current branch.\n"
                        "         Commit or stash them before merging.\n", staged);
        vcIndexFree(curEntries, curCount);
        free(currentBranch);
        return -1;
    }

    // Load source branch index.
    char srcIndexPath[MAX_DIR_PATH];
    snprintf(srcIndexPath, sizeof(srcIndexPath),
             "%s/.vc/branches/%s/index", vcTopDir, srcBranch);
    IndexEntry *srcEntries = NULL;
    int srcCount = 0;
    FILE *sf = fopen(srcIndexPath, "r");
    if (sf) {
        char line[4096];
        while (fgets(line, sizeof(line), sf) != NULL) {
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            if (len == 0) continue;
            char *path  = strtok(line, "|");
            char *state = strtok(NULL, "|");
            if (!path || !state) continue;
            IndexEntry *grown = realloc(srcEntries,
                                        (size_t)(srcCount+1) * sizeof(IndexEntry));
            if (!grown) break;
            srcEntries = grown;
            srcEntries[srcCount].path  = strdup(path);
            srcEntries[srcCount].state = INDEX_COMMITTED;
            srcEntries[srcCount].mtime = 0;
            srcEntries[srcCount].size  = 0;
            srcCount++;
        }
        fclose(sf);
    }

    printf("\nMerging branch '%s' → '%s'\n", srcBranch, currentBranch);
    printf("Conflict strategy: %s\n\n", preferTheirs ? "theirs" : "ours");

    int added = 0, updated = 0, conflicts = 0, skipped = 0;

    for (int i = 0; i < srcCount; i++) {
        const char *relPath = srcEntries[i].path;

        // Skip directory entries.
        size_t plen = strlen(relPath);
        if (plen > 0 && relPath[plen-1] == '/') continue;

        // Check if this file exists in the current branch index.
        int curIdx = vcIndexFind(curEntries, curCount, relPath);
        char dstPath[MAX_DIR_PATH];
        snprintf(dstPath, sizeof(dstPath), "%s/%s", vcTopDir, relPath);

        if (curIdx < 0) {
            // File only on source branch — add it.
            size_t srcSize = 0;
            char *srcBuf = extractFromBranch(srcBranch, relPath, &srcSize);
            if (srcBuf == NULL) { skipped++; continue; }

            writeFileContents(dstPath, srcBuf, srcSize);
            free(srcBuf);

            // Add to current index as staged.
            curEntries = vcIndexStage(curEntries, &curCount, dstPath);
            printf("  added     %s\n", relPath);
            added++;

        } else {
            // File exists on both branches — compare content.
            size_t srcSize = 0, curSize = 0;
            char *srcBuf = extractFromBranch(srcBranch,  relPath, &srcSize);
            char *curBuf = readFileContents(dstPath, &curSize);

            if (srcBuf == NULL) { free(curBuf); skipped++; continue; }

            bool identical = (srcSize == curSize &&
                              curBuf != NULL &&
                              memcmp(srcBuf, curBuf, srcSize) == 0);

            if (identical) {
                free(srcBuf); free(curBuf);
                skipped++;
                continue;   // No change needed.
            }

            // Files differ — attempt a 3-way line-level merge.
            // First extract the common ancestor (base branch commit).
            char tmpOurs[MAX_DIR_PATH], tmpBase[MAX_DIR_PATH];
            char tmpTheirs[MAX_DIR_PATH], tmpMerged[MAX_DIR_PATH];
            snprintf(tmpOurs,   sizeof(tmpOurs),   "/tmp/.vcm_ours_%d",   (int)getpid());
            snprintf(tmpBase,   sizeof(tmpBase),   "/tmp/.vcm_base_%d",   (int)getpid());
            snprintf(tmpTheirs, sizeof(tmpTheirs), "/tmp/.vcm_theirs_%d", (int)getpid());
            snprintf(tmpMerged, sizeof(tmpMerged), "/tmp/.vcm_merged_%d", (int)getpid());

            // Write ours (current working file) and theirs to temp files.
            writeFileContents(tmpOurs,   curBuf, curSize);
            writeFileContents(tmpTheirs, srcBuf, srcSize);

            // Try to extract base version from common ancestor.
            // Use the current branch's last committed version as base.
            size_t baseSize = 0;
            char *baseBuf = extractFromBranch(currentBranch, relPath, &baseSize);
            bool hasBase = (baseBuf != NULL);

            int mergeResult = -1;
            if (hasBase) {
                writeFileContents(tmpBase, baseBuf, baseSize);
                free(baseBuf);
                mergeResult = vc_merge3(tmpOurs, tmpBase, tmpTheirs,
                                        tmpMerged,
                                        currentBranch, srcBranch);
            }

            if (mergeResult == 0) {
                // Clean 3-way merge — use merged result.
                size_t mSize = 0;
                char *mBuf = readFileContents(tmpMerged, &mSize);
                if (mBuf) {
                    writeFileContents(dstPath, mBuf, mSize);
                    free(mBuf);
                }
                curEntries[curIdx].state = INDEX_STAGED;
                printf("  merged    %s\n", relPath);
                updated++;
            } else if (mergeResult == 1) {
                // Conflict markers written — use merged file with markers.
                size_t mSize = 0;
                char *mBuf = readFileContents(tmpMerged, &mSize);
                if (mBuf) {
                    writeFileContents(dstPath, mBuf, mSize);
                    free(mBuf);
                }
                printf("  conflict  %s  (conflict markers added — resolve and vc add)\n",
                       relPath);
                conflicts++;
            } else {
                // No base or merge error — fall back to conflict strategy.
                if (preferTheirs) {
                    writeFileContents(dstPath, srcBuf, srcSize);
                    curEntries[curIdx].state = INDEX_STAGED;
                    printf("  updated   %s  (theirs — no base for merge)\n", relPath);
                    updated++;
                } else {
                    char mergePath[MAX_DIR_PATH];
                    snprintf(mergePath, sizeof(mergePath),
                             "%s/%s.theirs", vcTopDir, relPath);
                    writeFileContents(mergePath, srcBuf, srcSize);
                    printf("  conflict  %s  (kept ours, theirs → %s.theirs)\n",
                           relPath, relPath);
                    conflicts++;
                }
            }

            // Clean up temp files.
            remove(tmpOurs); remove(tmpBase);
            remove(tmpTheirs); remove(tmpMerged);
            free(srcBuf); free(curBuf);
        }
    }

    // Save updated index.
    vcIndexSave(curEntries, curCount);
    vcIndexFree(curEntries, curCount);
    vcIndexFree(srcEntries, srcCount);

    printf("\nMerge complete:\n");
    printf("  Added     : %d file(s)\n", added);
    printf("  Merged    : %d file(s)\n", updated);
    if (conflicts > 0) {
        printf("  Conflicts : %d file(s)  ← resolve then 'vc add' and 'vc commit'\n",
               conflicts);
        printf("\n  Conflict markers in affected files:\n");
        printf("    <<<<<<< %s  (your version)\n", currentBranch);
        printf("    |||||||  base\n");
        printf("    =======\n");
        printf("    >>>>>>>  (their version)\n");
    }
    printf("  Unchanged : %d file(s)\n", skipped);
    if (added + updated > 0)
        printf("\nRun 'vc commit --msg \"Merge %s\"' to record the merge.\n",
               srcBranch);
    free(currentBranch);
    return 0;
}
