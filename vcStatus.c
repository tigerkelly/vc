#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "vc.h"
#include "vcd.h"
#include "vcTransport.h"
#include "vcAuth.h"

// Forward declaration – defined at the bottom of this file.
static void vcStatusCollectUntracked(const char *dirPath,
                                     IndexEntry *entries, int count,
                                     char **list, int *listCount);

// -------------------------------------------------------------------
// Module-level state shared with the walk callback.
// -------------------------------------------------------------------
typedef struct {
    IndexEntry *entries;
    int         count;
    // Counters for the summary line.
    int         nStaged;
    int         nModified;
    int         nMissing;
} StatusCtx;

static StatusCtx _sCtx;

// -------------------------------------------------------------------
// relPath  –  strip vcTopDir prefix from a full path.
// Returns a pointer into the original string, never allocates.
// -------------------------------------------------------------------
// -------------------------------------------------------------------
// isModified  –  true if the file on disk has changed since the
// mtime stored in the index.
//
// mtime alone is unreliable at 1-second granularity — a file edited
// and saved within the same second as the last commit will have an
// identical mtime even though its content changed.  We also compare
// file size: if the size differs the file is definitely modified,
// regardless of mtime.  The index entry carries both mtime and size.
// -------------------------------------------------------------------
static bool isModified(const char *fullPath, time_t indexedMtime, off_t indexedSize) {
    struct stat st;
    if (stat(fullPath, &st) == -1)
        return false;
    // Size change is a definitive signal.
    if (st.st_size != indexedSize)
        return true;
    // Fall back to mtime for same-size edits (e.g. replacing one char).
    return st.st_mtime != indexedMtime;
}

// -------------------------------------------------------------------
// vcStatus  –  entry point called from vc.c
//
// Output mirrors the style of git status:
//
//   Project : myproject
//   Branch  : main
//
//   Remote  : 2 commits ahead (run 'vc pull')
//   Remote  : 1 commit behind (run 'vc push')
//   Remote  : up to date
//
//   Staged for commit:
//     staged     src/main.c
//
//   Modified but not staged (use 'vc add' to update):
//     modified   src/utils.c
//
//   Untracked files (use 'vc add' to track)
//     untracked  README.md
//
//   Missing files (deleted on disk but still in index):
//     missing    src/old.c
//
//   3 staged, 1 modified, 1 untracked, 1 missing
// -------------------------------------------------------------------
int vcStatus(bool remoteCheck) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // 1. Load the index.
    _sCtx.count     = vcIndexLoad(&_sCtx.entries);
    _sCtx.nStaged   = 0;
    _sCtx.nModified = 0;
    _sCtx.nMissing  = 0;

    if (_sCtx.count < 0) {
        fprintf(stderr, "vcStatus: failed to load index.\n");
        return -1;
    }

    // 2. Print remote info header.
    printf("\n");

    extern Config *config;
    if (config != NULL) {
        if (config->host[0] != '\0') {
            const char *login = config->vcdUser[0]   ? config->vcdUser
                              : "(not set)";
            printf("Remote  : %s@%s:%s\n",
                   login, config->host, config->repo);
        } else {
            printf("Remote  : (not configured — run 'vc config')\n");
        }
        printf("Private : %s", config->isPrivate ? "yes" : "no");
        if (config->isPrivate && config->owner[0])
            printf("  (owner: %s)", config->owner);
        if (config->isPrivate && config->allowedUsers[0])
            printf("  allowed: %s", config->allowedUsers);
        printf("\n");

        // 3. Optional remote comparison (--remote flag).
        if (remoteCheck && config->host[0] != '\0' &&
            config->vcdUser[0] != '\0' && config->repo[0] != '\0') {

            const char *vcdUser = config->vcdUser;
            int port = config->port > 0 ? config->port : VCD_DEFAULT_PORT;

            char password[256] = "";
            if (!vcAuthPrompt(vcTopDir, config->host, vcdUser,
                              password, sizeof(password))) {
                printf("Remote  : (skipped — no password)\n");
            } else {
                printf("Remote  : checking...");
                fflush(stdout);

                VcSession s;
                if (!vct_connect(&s, config->host, port, vcdUser, password)) {
                    printf("\rRemote  : (connection failed)          \n");
                } else {
                    char localData[MAX_DIR_PATH];
                    snprintf(localData, sizeof(localData),
                             "%s/.vc/data", vcTopDir);

                    int remoteOnly = 0, localOnly = 0;
                    int r = vct_remote_status(&s, config->repo,
                                             localData,
                                             &remoteOnly, &localOnly);
                    vct_disconnect(&s);

                    printf("\r");  // clear the "checking..." line
                    if (r == -999) {
                        printf("Remote  : (could not retrieve remote status)\n");
                    } else if (remoteOnly == 0 && localOnly == 0) {
                        printf("Remote  : up to date\n");
                    } else {
                        if (remoteOnly > 0)
                            printf("Remote  : %d commit%s ahead"
                                   " — run 'vc pull'\n",
                                   remoteOnly,
                                   remoteOnly == 1 ? "" : "s");
                        if (localOnly > 0)
                            printf("Remote  : %d commit%s behind"
                                   " — run 'vc push'\n",
                                   localOnly,
                                   localOnly == 1 ? "" : "s");
                    }
                }
                memset(password, 0, sizeof(password));
            }
        }

        printf("---\n\n");
    }

    // 3. Walk the committed/staged entries and detect on-disk changes.
    //    Build three lists: staged, modified, missing.
    //    We print them after the walk so each section is grouped.

    // Temporary string arrays for each category.
    // We allocate lazily – worst case is all files in one bucket.
    char **staged   = malloc((size_t)(_sCtx.count + 1) * sizeof(char *));
    char **modified = malloc((size_t)(_sCtx.count + 1) * sizeof(char *));
    char **missing  = malloc((size_t)(_sCtx.count + 1) * sizeof(char *));

    if (staged == NULL || modified == NULL || missing == NULL) {
        fprintf(stderr, "vcStatus: out of memory.\n");
        free(staged); free(modified); free(missing);
        vcIndexFree(_sCtx.entries, _sCtx.count);
        return -1;
    }

    for (int i = 0; i < _sCtx.count; i++) {
        char fullPath[MAX_DIR_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s/%s",
                 vcTopDir, _sCtx.entries[i].path);

        struct stat st;
        bool exists = (stat(fullPath, &st) == 0);

        if (!exists) {
            // File/dir was deleted on disk but still in the index.
            missing[_sCtx.nMissing++] = _sCtx.entries[i].path;
            continue;
        }

        // Directory entries (trailing '/') are never "modified" —
        // skip change detection for them entirely.
        size_t plen = strlen(_sCtx.entries[i].path);
        bool isDir = (plen > 0 && _sCtx.entries[i].path[plen-1] == '/');

        switch (_sCtx.entries[i].state) {
            case INDEX_STAGED:
                staged[_sCtx.nStaged++] = _sCtx.entries[i].path;
                if (!isDir &&
                    isModified(fullPath, _sCtx.entries[i].mtime,
                                         _sCtx.entries[i].size)) {
                    modified[_sCtx.nModified++] = _sCtx.entries[i].path;
                }
                break;

            case INDEX_COMMITTED:
                if (!isDir &&
                    isModified(fullPath, _sCtx.entries[i].mtime,
                                         _sCtx.entries[i].size)) {
                    _sCtx.entries[i].state = INDEX_MODIFIED;
                    modified[_sCtx.nModified++] = _sCtx.entries[i].path;
                }
                break;

            case INDEX_MODIFIED:
                modified[_sCtx.nModified++] = _sCtx.entries[i].path;
                break;
        }
    }

    // 4. Print staged section.
    if (_sCtx.nStaged > 0) {
        printf("Staged for commit:\n");
        for (int i = 0; i < _sCtx.nStaged; i++)
            printf("  \033[0;32mstaged     %s\033[0m\n", staged[i]);
        printf("\n");
    }

    // 5. Print modified section.
    if (_sCtx.nModified > 0) {
        printf("Modified since last add  (use 'vc add <file>' to restage):\n");
        for (int i = 0; i < _sCtx.nModified; i++)
            printf("  \033[0;33mmodified   %s\033[0m\n", modified[i]);
        printf("\n");
    }

    // 6. Walk the project tree to find untracked files.
    char **untracked = malloc(4096 * sizeof(char *));
    int nUntracked = 0;

    if (untracked == NULL) {
        fprintf(stderr, "vcStatus: out of memory.\n");
        free(staged); free(modified); free(missing);
        vcIndexFree(_sCtx.entries, _sCtx.count);
        return -1;
    }

    vcStatusCollectUntracked(vcTopDir, _sCtx.entries, _sCtx.count,
                              untracked, &nUntracked);

    if (nUntracked > 0) {
        printf("Untracked files  (use 'vc add <file>' to track):\n");
        for (int i = 0; i < nUntracked; i++)
            printf("  \033[0;90muntracked  %s\033[0m\n", untracked[i]);
        printf("\n");
        // Free the strdup'd strings.
        for (int i = 0; i < nUntracked; i++)
            free(untracked[i]);
    }

    // 7. Print missing section.
    if (_sCtx.nMissing > 0) {
        printf("Missing files  (deleted on disk, use 'vc delete <file>' to remove):\n");
        for (int i = 0; i < _sCtx.nMissing; i++)
            printf("  \033[0;31mmissing    %s\033[0m\n", missing[i]);
        printf("\n");
    }

    // 8. Nothing at all?
    if (_sCtx.nStaged == 0 && _sCtx.nModified == 0 &&
        nUntracked == 0 && _sCtx.nMissing == 0) {
        printf("Nothing to report. Working tree is clean.\n\n");
    }

    // 9. Summary line.
    printf("Summary: %d staged, %d modified, %d untracked, %d missing\n\n",
           _sCtx.nStaged, _sCtx.nModified, nUntracked, _sCtx.nMissing);

    // 10. If modified files exist, persist the updated state to the index.
    if (_sCtx.nModified > 0)
        vcIndexSave(_sCtx.entries, _sCtx.count);

    free(staged);
    free(modified);
    free(missing);
    free(untracked);
    vcIndexFree(_sCtx.entries, _sCtx.count);
    return 0;
}

// -------------------------------------------------------------------
// dirHasTrackedFiles  –  return true if any file anywhere under
// dirPath has an entry in the index.  Used to decide whether to
// show a directory as a whole or recurse into it.
// -------------------------------------------------------------------
static bool dirHasTrackedFiles(const char *dirPath,
                                IndexEntry *entries, int count) {
    DIR *dir = opendir(dirPath);
    if (dir == NULL) return false;

    struct dirent *entry;
    struct stat st;
    char path[MAX_DIR_PATH];
    bool found = false;

    while (!found && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0  ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, VC_DIR) == 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dirPath, entry->d_name);
        if (stat(path, &st) == -1) continue;

        if (S_ISDIR(st.st_mode)) {
            found = dirHasTrackedFiles(path, entries, count);
        } else {
            // Build relative path and look it up in the index.
            char canonical[MAX_DIR_PATH];
            const char *rel = path;
            if (realpath(path, canonical) != NULL) {
                static char canonTop[MAX_DIR_PATH];
                static bool topResolved = false;
                if (!topResolved) {
                    if (realpath(vcTopDir, canonTop) == NULL)
                        snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);
                    topResolved = true;
                }
                size_t topLen = strlen(canonTop);
                if (strncmp(canonical, canonTop, topLen) == 0) {
                    rel = canonical + topLen;
                    if (*rel == '/') rel++;
                }
            }
            if (vcIndexFind(entries, count, rel) >= 0)
                found = true;
        }
    }

    closedir(dir);
    return found;
}

// -------------------------------------------------------------------
// vcStatusCollectUntracked  –  recursive directory walker that builds
// a list of files/directories not found in the index.
//
// Directory handling:
//   - Empty directory             → listed as untracked (dir/)
//   - All contents untracked      → listed as untracked (dir/) not each file
//   - Some contents tracked       → recurse, list only untracked files inside
//
// Uses strdup so the caller must free each string in the list.
// -------------------------------------------------------------------
static void vcStatusCollectUntracked(const char *dirPath,
                               IndexEntry *entries, int count,
                               char **list, int *listCount) {
    DIR *dir = opendir(dirPath);
    if (dir == NULL) return;

    struct dirent *entry;
    struct stat st;
    char path[MAX_DIR_PATH];

    while ((entry = readdir(dir)) != NULL) {
        // Skip dot entries and the .vc directory.
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, VC_DIR) == 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dirPath, entry->d_name);

        if (stat(path, &st) == -1)
            continue;

        // Check ignore patterns.
        bool ignored = false;
        for (int i = 0; i < MAX_IGNORE; i++) {
            if (ignores[i].pattern == NULL) continue;
            if (ignores[i].pattern[0] == '\0') continue;
            if (vc_fnmatch(ignores[i].pattern, entry->d_name) == 0) {
                ignored = true;
                break;
            }
        }
        if (ignored) continue;

        if (S_ISDIR(st.st_mode)) {
            // Compute the relative path of this directory for display.
            const char *rel = path;
            size_t topLen = strlen(vcTopDir);
            if (strncmp(path, vcTopDir, topLen) == 0) {
                rel = path + topLen;
                if (*rel == '/') rel++;
            }

            // Build the index key for this directory (trailing slash).
            char dirKey[MAX_DIR_PATH];
            snprintf(dirKey, sizeof(dirKey), "%s/", rel);

            // Check if the directory itself is in the index.
            bool inIndex = (vcIndexFind(entries, count, dirKey) >= 0);

            if (inIndex) {
                // Directory is staged/committed — not untracked.
                // Still recurse in case it contains untracked files.
                vcStatusCollectUntracked(path, entries, count,
                                         list, listCount);
            } else if (!dirHasTrackedFiles(path, entries, count)) {
                // No tracked files inside and not indexed itself —
                // show the whole directory as a single untracked entry.
                list[(*listCount)++] = strdup(dirKey);
            } else {
                // Some files inside are already tracked — recurse and
                // show only the individual untracked files within.
                vcStatusCollectUntracked(path, entries, count,
                                         list, listCount);
            }
        } else {
            // Regular file – resolve to a canonical relative path
            // so it matches exactly what was stored by vcIndexStage.
            char canonical[MAX_DIR_PATH];
            const char *rel;

            if (realpath(path, canonical) != NULL) {
                static char canonTop[MAX_DIR_PATH];
                static bool topResolved = false;
                if (!topResolved) {
                    if (realpath(vcTopDir, canonTop) == NULL)
                        snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);
                    topResolved = true;
                }
                size_t topLen = strlen(canonTop);
                rel = canonical;
                if (strncmp(canonical, canonTop, topLen) == 0) {
                    rel = canonical + topLen;
                    if (*rel == '/') rel++;
                }
            } else {
                rel = path;
                size_t topLen = strlen(vcTopDir);
                if (strncmp(path, vcTopDir, topLen) == 0) {
                    rel = path + topLen;
                    if (*rel == '/') rel++;
                }
            }

            if (vcIndexFind(entries, count, rel) < 0) {
                list[(*listCount)++] = strdup(rel);
            }
        }
    }

    closedir(dir);
}
