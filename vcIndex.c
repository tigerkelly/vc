
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "vc.h"

// -------------------------------------------------------------------
// The index file lives at  .vc/branches/<branch>/index
// so every branch maintains its own independent staging area.
//
// Each line has four pipe-separated fields:
//
//   <relative-path>|<state>|<mtime>|<size>
//
// States:
//   staged     – added since the last commit, will go into next commit
//   committed  – included in the most recent commit, unchanged since
//   modified   – committed before but changed on disk since then
//
// Example:
//   src/main.c|staged|1747084800|4096
//   README.md|committed|1747001200|512
// -------------------------------------------------------------------

#define INDEX_SEP      "|"
#define MAX_INDEX_LINE 4096

// -------------------------------------------------------------------
// indexFilePath  –  return the path to the active branch's index.
// Fills buf with  .vc/branches/<current-branch>/index
// Falls back to  .vc/index  if the branch system is not yet set up
// (e.g. during init before HEAD exists).
// -------------------------------------------------------------------
static void indexFilePath(char *buf, size_t sz) {
    char *branch = vcBranchCurrentName();
    if (branch != NULL && branch[0] != '\0') {
        snprintf(buf, sz, ".vc/branches/%s/index", branch);
        free(branch);
    } else {
        free(branch);
        snprintf(buf, sz, ".vc/index");   // fallback
    }
}

// -------------------------------------------------------------------
// indexPath  –  given any path (absolute, relative, or containing ./)
// return the canonical path relative to vcTopDir for index storage.
// e.g.  /home/user/proj/./src/main.c  →  src/main.c
//        ./src/main.c                  →  src/main.c
//
// Uses realpath() to resolve symlinks and . / .. components so that
// every path variant for the same file always maps to the same key.
// Returns a pointer to a static buffer – caller must strdup if needed.
// -------------------------------------------------------------------
static const char *indexPath(const char *fullPath) {
    static char canonical[MAX_DIR_PATH];
    static char result[MAX_DIR_PATH];

    // Resolve to an absolute canonical path.
    if (realpath(fullPath, canonical) == NULL) {
        // realpath failed (e.g. file doesn't exist yet) – fall back to
        // simple prefix strip on the original path.
        size_t topLen = strlen(vcTopDir);
        if (strncmp(fullPath, vcTopDir, topLen) == 0) {
            const char *rel = fullPath + topLen;
            if (*rel == '/') rel++;
            return rel;
        }
        return fullPath;
    }

    // Strip the canonical vcTopDir prefix.
    // Also canonicalise vcTopDir itself once via realpath.
    static char canonTop[MAX_DIR_PATH];
    static bool topResolved = false;
    if (!topResolved) {
        if (realpath(vcTopDir, canonTop) == NULL)
            snprintf(canonTop, sizeof(canonTop), "%s", vcTopDir);
        topResolved = true;
    }

    size_t topLen = strlen(canonTop);
    if (strncmp(canonical, canonTop, topLen) == 0) {
        const char *rel = canonical + topLen;
        if (*rel == '/') rel++;
        snprintf(result, sizeof(result), "%s", rel);
        return result;
    }

    // Path is outside the project tree.
    return canonical;
}

// -------------------------------------------------------------------
// vcIndexLoad  –  read the entire index into a heap-allocated array.
// Returns the number of entries loaded, or -1 on error.
// Caller must free each entry's path and the array itself via
// vcIndexFree().
// -------------------------------------------------------------------
int vcIndexLoad(IndexEntry **entriesOut) {
    *entriesOut = NULL;

    char indexFile[MAX_DIR_PATH];
    indexFilePath(indexFile, sizeof(indexFile));

    FILE *f = fopen(indexFile, "r");
    if (f == NULL) {
        // No index yet – that is fine, treat as empty.
        return 0;
    }

    // Count lines first so we can allocate exactly.
    int count = 0;
    char line[MAX_INDEX_LINE];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] != '\0' && line[0] != '\n')
            count++;
    }
    rewind(f);

    IndexEntry *entries = calloc((size_t)count, sizeof(IndexEntry));
    if (entries == NULL) {
        fclose(f);
        return -1;
    }

    int idx = 0;
    while (fgets(line, sizeof(line), f) != NULL && idx < count) {
        // Strip trailing newline.
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        // Parse  path|state|mtime|size
        char *path  = strtok(line, INDEX_SEP);
        char *state = strtok(NULL, INDEX_SEP);
        char *mtime = strtok(NULL, INDEX_SEP);
        char *size  = strtok(NULL, INDEX_SEP);

        if (path == NULL || state == NULL || mtime == NULL)
            continue;    // malformed line, skip

        entries[idx].path  = strdup(path);
        entries[idx].mtime = (time_t)atol(mtime);
        entries[idx].size  = (size != NULL) ? (off_t)atol(size) : 0;

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
// vcIndexSave  –  write the entire index back to disk atomically
// by writing a temp file then renaming over the old one.
// -------------------------------------------------------------------
int vcIndexSave(IndexEntry *entries, int count) {
    char indexFile[MAX_DIR_PATH];
    indexFilePath(indexFile, sizeof(indexFile));

    // Write to a temp file alongside the real index then rename atomically.
    char tmpPath[MAX_DIR_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", indexFile);

    FILE *f = fopen(tmpPath, "w");
    if (f == NULL) {
        fprintf(stderr, "vcIndexSave: cannot open %s: %s\n",
                tmpPath, strerror(errno));
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const char *stateStr;
        switch (entries[i].state) {
            case INDEX_STAGED:    stateStr = "staged";    break;
            case INDEX_MODIFIED:  stateStr = "modified";  break;
            default:              stateStr = "committed";  break;
        }
        fprintf(f, "%s|%s|%ld|%ld\n",
                entries[i].path, stateStr,
                (long)entries[i].mtime, (long)entries[i].size);
    }

    fclose(f);

    if (rename(tmpPath, indexFile) != 0) {
        fprintf(stderr, "vcIndexSave: rename failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

// -------------------------------------------------------------------
// vcIndexFree  –  release memory allocated by vcIndexLoad.
// -------------------------------------------------------------------
void vcIndexFree(IndexEntry *entries, int count) {
    if (entries == NULL) return;
    for (int i = 0; i < count; i++)
        free(entries[i].path);
    free(entries);
}

// -------------------------------------------------------------------
// vcIndexFind  –  return the index of an entry by path, or -1.
// -------------------------------------------------------------------
int vcIndexFind(IndexEntry *entries, int count, const char *relPath) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].path, relPath) == 0)
            return i;
    }
    return -1;
}

// -------------------------------------------------------------------
// vcIndexStage  –  mark a single file as staged in the index.
// If it is already there, update its state and mtime.
// If it is new, append it.
// entries / count may be reallocated; always use the returned pointer.
// -------------------------------------------------------------------
IndexEntry *vcIndexStage(IndexEntry *entries, int *count, const char *fullPath) {
    struct stat st;
    if (stat(fullPath, &st) == -1) {
        fprintf(stderr, "vcIndexStage: cannot stat %s: %s\n",
                fullPath, strerror(errno));
        return entries;
    }

    // Directories are stored with a trailing '/' to distinguish them
    // from files and so vcCommit can add them as zip directory entries.
    const char *rel = indexPath(fullPath);

    // Build the key used for lookup and storage.
    char relKey[MAX_DIR_PATH];
    if (S_ISDIR(st.st_mode)) {
        // Append trailing slash if not already present.
        size_t len = strlen(rel);
        if (len > 0 && rel[len-1] == '/')
            snprintf(relKey, sizeof(relKey), "%s", rel);
        else
            snprintf(relKey, sizeof(relKey), "%s/", rel);
    } else {
        snprintf(relKey, sizeof(relKey), "%s", rel);
    }

    int idx = vcIndexFind(entries, *count, relKey);

    if (idx >= 0) {
        // Already tracked – update state, mtime, and size.
        entries[idx].state = INDEX_STAGED;
        entries[idx].mtime = S_ISDIR(st.st_mode) ? 0 : st.st_mtime;
        entries[idx].size  = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    } else {
        // New entry – grow the array by one.
        IndexEntry *grown = realloc(entries,
                                    (size_t)(*count + 1) * sizeof(IndexEntry));
        if (grown == NULL) {
            fprintf(stderr, "vcIndexStage: out of memory\n");
            return entries;
        }
        entries = grown;
        entries[*count].path  = strdup(relKey);
        entries[*count].state = INDEX_STAGED;
        entries[*count].mtime = S_ISDIR(st.st_mode) ? 0 : st.st_mtime;
        entries[*count].size  = S_ISDIR(st.st_mode) ? 0 : st.st_size;
        (*count)++;
    }

    return entries;
}

// -------------------------------------------------------------------
// vcIndexMarkCommitted  –  after a successful commit, flip every
// staged entry to committed and update its mtime.
// -------------------------------------------------------------------
void vcIndexMarkCommitted(IndexEntry *entries, int count) {
    for (int i = 0; i < count; i++) {
        if (entries[i].state == INDEX_STAGED) {
            entries[i].state = INDEX_COMMITTED;
            // Refresh mtime from disk.
            struct stat st;
            char fullPath[MAX_DIR_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s/%s",
                     vcTopDir, entries[i].path);
            if (stat(fullPath, &st) == 0) {
                entries[i].mtime = st.st_mtime;
                entries[i].size  = st.st_size;
            }
        }
    }
}
