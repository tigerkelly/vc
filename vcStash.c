
#include <stdio.h>
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

// -------------------------------------------------------------------
// Stash layout:
//   .vc/stash/                    directory
//   .vc/stash/YYYYMMDD_HHMMSS/   one stash entry
//   .vc/stash/YYYYMMDD_HHMMSS/files.zip   staged + modified files
//   .vc/stash/YYYYMMDD_HHMMSS/index       snapshot of the branch index
//   .vc/stash/YYYYMMDD_HHMMSS/message     optional description
//
// vcStash (push):  zip all staged+modified files, save the index,
//                  then restore index to all-committed state.
// vcStash pop:     restore files from the stash zip, restore index.
// vcStash list:    show available stashes.
// vcStash drop:    delete a stash entry.
// -------------------------------------------------------------------

#define VC_STASH_DIR  ".vc/stash"

static void ensureStashDir(void) {
    char path[MAX_DIR_PATH];
    snprintf(path, sizeof(path), "%s/%s", vcTopDir, VC_STASH_DIR);
    struct stat st;
    if (stat(path, &st) != 0)
        mkdir(path, 0755);
}

static void makeStashName(char *buf, size_t sz) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(buf, sz, "%s", ts);
}

// -------------------------------------------------------------------
// stashPush  –  save staged/modified files and clean the index.
// -------------------------------------------------------------------
static int stashPush(const char *message) {

    // Load index.
    IndexEntry *entries = NULL;
    int count = vcIndexLoad(&entries);

    // Collect staged and modified files.
    int dirty = 0;
    for (int i = 0; i < count; i++)
        if (entries[i].state == INDEX_STAGED ||
            entries[i].state == INDEX_MODIFIED)
            dirty++;

    if (dirty == 0) {
        printf("Nothing to stash — no staged or modified files.\n");
        vcIndexFree(entries, count);
        return 0;
    }

    ensureStashDir();

    // Build stash directory name.
    char stashName[32];
    makeStashName(stashName, sizeof(stashName));
    char stashDir[MAX_DIR_PATH];
    snprintf(stashDir, sizeof(stashDir), "%s/%s/%s",
             vcTopDir, VC_STASH_DIR, stashName);

    // If same second, append _2 etc.
    struct stat st;
    if (stat(stashDir, &st) == 0) {
        for (int n = 2; n < 100; n++) {
            snprintf(stashDir, sizeof(stashDir), "%s/%s/%s_%d",
                     vcTopDir, VC_STASH_DIR, stashName, n);
            if (stat(stashDir, &st) != 0) break;
        }
    }
    mkdir(stashDir, 0755);

    // Write message file.
    if (message && message[0]) {
        char msgPath[MAX_DIR_PATH];
        snprintf(msgPath, sizeof(msgPath), "%s/message", stashDir);
        FILE *mf = fopen(msgPath, "w");
        if (mf) { fprintf(mf, "%s\n", message); fclose(mf); }
    }

    // Copy the current index to the stash.
    char indexSrc[MAX_DIR_PATH];
    char indexDst[MAX_DIR_PATH];
    char *branch = vcBranchCurrentName();
    snprintf(indexSrc, sizeof(indexSrc), "%s/.vc/branches/%s/index",
             vcTopDir, branch);
    snprintf(indexDst, sizeof(indexDst), "%s/index", stashDir);
    free(branch);

    char cmd[MAX_DIR_PATH * 2 + 16];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", indexSrc, indexDst);
    system(cmd);

    // Create a zip of all staged/modified files.
    char zipPath[MAX_DIR_PATH];
    snprintf(zipPath, sizeof(zipPath), "%s/files.zip", stashDir);

    int zipErr = 0;
    zip_t *za = zip_open(zipPath, ZIP_CREATE | ZIP_EXCL, &zipErr);
    if (za == NULL) {
        fprintf(stderr, "vcStash: cannot create stash archive.\n");
        vcIndexFree(entries, count);
        return -1;
    }

    int saved = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].state != INDEX_STAGED &&
            entries[i].state != INDEX_MODIFIED)
            continue;

        char fullPath[MAX_DIR_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, entries[i].path);

        zip_source_t *src = zip_source_file(za, fullPath, 0, -1);
        if (src == NULL) continue;
        if (zip_file_add(za, entries[i].path, src, ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(src);
            continue;
        }
        saved++;
        printf("  stash  %s\n", entries[i].path);

        // Mark as committed in the live index so status shows clean.
        entries[i].state = INDEX_COMMITTED;
    }

    if (zip_close(za) != 0) {
        fprintf(stderr, "vcStash: error closing stash archive.\n");
        vcIndexFree(entries, count);
        return -1;
    }

    // Save updated index (all dirty files now show as committed).
    vcIndexSave(entries, count);
    vcIndexFree(entries, count);

    printf("\nStashed %d file(s) → %s/%s\n",
           saved, VC_STASH_DIR, stashName);
    if (message && message[0])
        printf("Message: %s\n", message);
    printf("Working tree is now clean.\n");
    return 0;
}

// -------------------------------------------------------------------
// stashPop  –  restore the most recent (or named) stash.
// -------------------------------------------------------------------
static int stashPop(const char *stashName, bool drop) {

    char stashBase[MAX_DIR_PATH];
    snprintf(stashBase, sizeof(stashBase), "%s/%s", vcTopDir, VC_STASH_DIR);

    char stashDir[MAX_DIR_PATH];

    if (stashName == NULL) {
        // Find the most recent stash (newest name = lexicographic max).
        DIR *dir = opendir(stashBase);
        if (dir == NULL) {
            printf("No stashes found.\n");
            return 0;
        }
        char newest[256] = "";
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (strcmp(de->d_name, newest) > 0)
                snprintf(newest, 256, "%s", de->d_name);
        }
        closedir(dir);
        if (newest[0] == '\0') {
            printf("No stashes found.\n");
            return 0;
        }
        snprintf(stashDir, sizeof(stashDir), "%s/%s", stashBase, newest);
    } else {
        snprintf(stashDir, sizeof(stashDir), "%s/%s", stashBase, stashName);
    }

    struct stat st;
    if (stat(stashDir, &st) != 0) {
        fprintf(stderr, "vcStash: stash '%s' not found.\n",
                stashName ? stashName : "(latest)");
        return -1;
    }

    char zipPath[MAX_DIR_PATH];
    snprintf(zipPath, sizeof(zipPath), "%s/files.zip", stashDir);
    char indexPath[MAX_DIR_PATH];
    snprintf(indexPath, sizeof(indexPath), "%s/index", stashDir);

    // Restore index from stash.
    char *branch = vcBranchCurrentName();
    char liveIndex[MAX_DIR_PATH];
    snprintf(liveIndex, sizeof(liveIndex), "%s/.vc/branches/%s/index",
             vcTopDir, branch);
    free(branch);

    char cmd[MAX_DIR_PATH * 2 + 16];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", indexPath, liveIndex);
    system(cmd);

    // Restore files from the stash zip.
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) {
        fprintf(stderr, "vcStash: cannot open stash archive.\n");
        return -1;
    }

    zip_int64_t n = zip_get_num_entries(za, 0);
    int restored = 0;

    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (name == NULL) continue;

        zip_close(za);
        if (vcRestoreFileFromZip(zipPath, name)) {
            printf("  pop  %s\n", name);
            restored++;
        }
        za = zip_open(zipPath, ZIP_RDONLY, &err);
        if (za == NULL) break;
    }
    if (za) zip_close(za);

    printf("\nRestored %d file(s) from stash.\n", restored);

    // Drop the stash entry if requested (pop) or keep it (apply).
    if (drop) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stashDir);
        system(cmd);
        printf("Stash entry removed.\n");
    }

    return 0;
}

// -------------------------------------------------------------------
// stashList  –  show all stash entries.
// -------------------------------------------------------------------
static int stashList(void) {
    char stashBase[MAX_DIR_PATH];
    snprintf(stashBase, sizeof(stashBase), "%s/%s", vcTopDir, VC_STASH_DIR);

    DIR *dir = opendir(stashBase);
    if (dir == NULL) {
        printf("No stashes found.\n");
        return 0;
    }

    // Collect names.
    char names[256][256];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < 256) {
        if (de->d_name[0] == '.') continue;
        snprintf(names[count++], 256, "%s", de->d_name);
    }
    closedir(dir);

    if (count == 0) {
        printf("No stashes found.\n");
        return 0;
    }

    // Sort newest-first.
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[j], names[i]) > 0) {
                char tmp[256];
                strncpy(tmp, names[i], 255); tmp[255] = '\0';
                strncpy(names[i], names[j], 255); names[i][255] = '\0';
                strncpy(names[j], tmp,      255); names[j][255] = '\0';
            }

    printf("Stashes (%d):\n\n", count);
    for (int i = 0; i < count; i++) {
        char msgPath[MAX_DIR_PATH];
        snprintf(msgPath, sizeof(msgPath), "%s/%s/message",
                 stashBase, names[i]);
        char msg[128] = "";
        FILE *mf = fopen(msgPath, "r");
        if (mf) {
            if (fgets(msg, sizeof(msg), mf)) {
                size_t l = strlen(msg);
                if (l > 0 && msg[l-1] == '\n') msg[--l] = '\0';
            }
            fclose(mf);
        }
        printf("  %-24s  %s\n", names[i], msg);
    }
    printf("\n");
    return 0;
}

// -------------------------------------------------------------------
// stashDrop  –  delete a stash entry.
// -------------------------------------------------------------------
static int stashDrop(const char *stashName) {
    char stashDir[MAX_DIR_PATH];
    snprintf(stashDir, sizeof(stashDir), "%s/%s/%s",
             vcTopDir, VC_STASH_DIR, stashName);
    struct stat st;
    if (stat(stashDir, &st) != 0) {
        fprintf(stderr, "vcStash: stash '%s' not found.\n", stashName);
        return -1;
    }
    char cmd[MAX_DIR_PATH + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stashDir);
    system(cmd);
    printf("Dropped stash: %s\n", stashName);
    return 0;
}

// -------------------------------------------------------------------
// vcStash  –  entry point.
//
// Usage:
//   vc stash                       Save staged/modified files, clean index
//   vc stash --msg "description"   Stash with a message
//   vc stash pop                   Restore most recent stash and remove it
//   vc stash apply                 Restore most recent stash, keep it
//   vc stash pop <name>            Restore named stash
//   vc stash list                  List all stashes
//   vc stash drop <name>           Delete a stash entry
// -------------------------------------------------------------------
int vcStash(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    if (argc < 3) {
        return stashPush(NULL);
    }

    if (strcmp(argv[2], "pop") == 0) {
        const char *name = (argc >= 4) ? argv[3] : NULL;
        return stashPop(name, true);
    }
    if (strcmp(argv[2], "apply") == 0) {
        const char *name = (argc >= 4) ? argv[3] : NULL;
        return stashPop(name, false);
    }
    if (strcmp(argv[2], "list") == 0) {
        return stashList();
    }
    if (strcmp(argv[2], "drop") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcStash: drop requires a stash name.\n"
                            "         Use 'vc stash list' to see available stashes.\n");
            return -1;
        }
        return stashDrop(argv[3]);
    }
    if (strcmp(argv[2], "--msg") == 0) {
        const char *msg = (argc >= 4) ? argv[3] : NULL;
        return stashPush(msg);
    }
    // Bare message without flag?
    return stashPush(argv[2]);
}
