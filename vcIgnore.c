
#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "vc.h"

// -------------------------------------------------------------------
// vcIgnore  –  manage .vcignore patterns.
//
// Usage:
//   vc ignore                  List current patterns
//   vc ignore <pattern>        Add a pattern
//   vc ignore --delete <pat>   Remove a pattern
//   vc ignore --check <file>   Test whether a file would be ignored
// -------------------------------------------------------------------

static char ignorePath[MAX_DIR_PATH];

static void buildIgnorePath(void) {
    snprintf(ignorePath, sizeof(ignorePath), "%s/%s", vcTopDir, VC_IGNORE);
}

// Read all lines from .vcignore into lines[] array.
// Returns count. Caller must free each entry.
static int readLines(char **lines, int max) {
    buildIgnorePath();
    FILE *f = fopen(ignorePath, "r");
    if (f == NULL) return 0;
    int count = 0;
    char buf[512];
    while (count < max && fgets(buf, sizeof(buf), f) != NULL) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        if (len == 0) continue;
        lines[count++] = strdup(buf);
    }
    fclose(f);
    return count;
}

static int writeLines(char **lines, int count) {
    buildIgnorePath();
    FILE *f = fopen(ignorePath, "w");
    if (f == NULL) {
        fprintf(stderr, "vcIgnore: cannot write '%s': %s\n",
                ignorePath, strerror(errno));
        return -1;
    }
    for (int i = 0; i < count; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
    return 0;
}

static void freeLines(char **lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
}

int vcIgnore(int argc, char *argv[]) {

    vcLog("%s\n", __func__);
    buildIgnorePath();

    // vc ignore  –  list patterns.
    if (argc < 3) {
        char *lines[512];
        int count = readLines(lines, 512);
        if (count == 0) {
            printf("No ignore patterns set. Use 'vc ignore <pattern>' to add one.\n");
            return 0;
        }
        printf(".vcignore patterns (%d):\n\n", count);
        for (int i = 0; i < count; i++) {
            if (lines[i][0] == '#')
                printf("  \033[0;90m%s\033[0m\n", lines[i]);  // comments grey
            else
                printf("  %s\n", lines[i]);
        }
        printf("\n");
        freeLines(lines, count);
        return 0;
    }

    // vc ignore --delete <pattern>
    if (strcmp(argv[2], "--delete") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcIgnore: --delete requires a pattern.\n");
            return -1;
        }
        const char *pattern = argv[3];
        char *lines[512];
        int count = readLines(lines, 512);
        int removed = 0;
        char *newLines[512];
        int newCount = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(lines[i], pattern) == 0) {
                free(lines[i]);
                removed++;
            } else {
                newLines[newCount++] = lines[i];
            }
        }
        if (removed == 0) {
            fprintf(stderr, "vcIgnore: pattern '%s' not found in .vcignore.\n",
                    pattern);
            freeLines(lines, count);
            return -1;
        }
        int rc = writeLines(newLines, newCount);
        freeLines(newLines, newCount);
        if (rc == 0)
            printf("Removed pattern: %s\n", pattern);
        return rc;
    }

    // vc ignore --check <file>
    if (strcmp(argv[2], "--check") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcIgnore: --check requires a filename.\n");
            return -1;
        }
        // Reload ignores and test.
        loadIgnores();
        const char *file = argv[3];
        // Strip path to basename for pattern matching.
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        bool ignored = false;
        for (int i = 0; i < MAX_IGNORE; i++) {
            if (ignores[i].pattern == NULL) break;
            if (ignores[i].pattern[0] == '#') continue;
            if (vc_fnmatch(ignores[i].pattern, base) == 0 ||
                vc_fnmatch(ignores[i].pattern, file) == 0) {
                ignored = true;
                printf("'%s' would be ignored  (matches pattern: %s)\n",
                       file, ignores[i].pattern);
                break;
            }
        }
        if (!ignored)
            printf("'%s' would NOT be ignored.\n", file);
        return 0;
    }

    // vc ignore <pattern>  –  add a pattern.
    const char *pattern = argv[2];

    // Check it's not already there.
    char *lines[512];
    int count = readLines(lines, 512);
    for (int i = 0; i < count; i++) {
        if (strcmp(lines[i], pattern) == 0) {
            printf("Pattern '%s' is already in .vcignore.\n", pattern);
            freeLines(lines, count);
            return 0;
        }
    }
    freeLines(lines, count);

    // Append to .vcignore.
    FILE *f = fopen(ignorePath, "a");
    if (f == NULL) {
        // Create it if it doesn't exist.
        f = fopen(ignorePath, "w");
        if (f == NULL) {
            fprintf(stderr, "vcIgnore: cannot create '%s': %s\n",
                    ignorePath, strerror(errno));
            return -1;
        }
    }
    fprintf(f, "%s\n", pattern);
    fclose(f);

    printf("Added ignore pattern: %s\n", pattern);
    return 0;
}
