
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <zip.h>

#include "vc.h"

// -------------------------------------------------------------------
// vcHistory  –  show commit history for a specific file.
//
// Usage:
//   vc history <file>            Show all commits that included this file
//   vc history <file> --count <n>  Show only n most recent
// -------------------------------------------------------------------
int vcHistory(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    if (argc < 3) {
        fprintf(stderr, "vcHistory: usage: vc history <file> [--count <n>]\n");
        return -1;
    }

    // Parse args.
    const char *fileName = NULL;
    int  limitCount = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i+1 < argc) {
            limitCount = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--count=", 8) == 0) {
            limitCount = atoi(argv[i] + 8);
        } else {
            fileName = argv[i];
        }
    }

    if (fileName == NULL) {
        fprintf(stderr, "vcHistory: no file specified.\n");
        return -1;
    }

    // Resolve relative path.
    char relPath[MAX_DIR_PATH];
    char fullPath[MAX_DIR_PATH];

    if (fileName[0] == '/')
        snprintf(fullPath, sizeof(fullPath), "%s", fileName);
    else
        snprintf(fullPath, sizeof(fullPath), "%s/%s", vcTopDir, fileName);

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
        // File may not exist on disk — use as-is.
        snprintf(relPath, sizeof(relPath), "%s", fileName);
    }

    // Collect all zips from data/ (search archived too).
    char dataDir[MAX_DIR_PATH];
    char archDir[MAX_DIR_PATH];
    snprintf(dataDir, sizeof(dataDir), "%s/.vc/data",    vcTopDir);
    snprintf(archDir, sizeof(archDir), "%s/.vc/archive", vcTopDir);

    // Build sorted zip list from both directories.
    typedef struct { char name[256]; char dir[MAX_DIR_PATH]; } ZipRef;
    ZipRef *zips = malloc(1024 * sizeof(ZipRef));
    if (zips == NULL) return -1;
    int zipCount = 0;

    const char *dirs[] = { dataDir, archDir, NULL };
    for (int d = 0; dirs[d]; d++) {
        DIR *dir = opendir(dirs[d]);
        if (dir == NULL) continue;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL && zipCount < 1024) {
            size_t len = strlen(de->d_name);
            if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0) {
                snprintf(zips[zipCount].name, 256, "%s", de->d_name);
                snprintf(zips[zipCount].dir, MAX_DIR_PATH, "%s", dirs[d]);
                zipCount++;
            }
        }
        closedir(dir);
    }

    // Sort newest-first.
    for (int i = 0; i < zipCount - 1; i++)
        for (int j = i + 1; j < zipCount; j++)
            if (strcmp(zips[j].name, zips[i].name) > 0) {
                ZipRef tmp = zips[i];
                zips[i]    = zips[j];
                zips[j]    = tmp;
            }

    // Search each zip for the file.
    printf("\nHistory for: %s\n\n", relPath);
    printf("  %-22s  %-24s  %s\n", "Archive", "Date", "Message");
    printf("  %-22s  %-24s  %s\n",
           "----------------------", "------------------------", "-------");

    int found = 0;
    for (int z = 0; z < zipCount; z++) {
        if (limitCount > 0 && found >= limitCount) break;

        char zipPath[MAX_DIR_PATH];
        snprintf(zipPath, sizeof(zipPath), "%s/%s", zips[z].dir, zips[z].name);

        int err = 0;
        zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
        if (za == NULL) continue;

        zip_int64_t idx = zip_name_locate(za, relPath, 0);
        if (idx < 0) { zip_close(za); continue; }

        // File found in this commit — read the manifest.
        char date[32]    = "(unknown)";
        char message[128] = "";
        zip_file_t *zf = zip_fopen(za, "MANIFEST.txt", 0);
        if (zf) {
            char buf[2048] = {0};
            zip_fread(zf, buf, sizeof(buf)-1);
            zip_fclose(zf);
            // Parse date and message.
            for (const char *p = buf; *p; ) {
                char line[2048];
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len >= sizeof(line)) len = sizeof(line)-1;
                snprintf(line, len+1, "%s", p);
                if (strncmp(line, "date", 4) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) { char *v = eq+1; while(*v==' ')v++;
                              snprintf(date, sizeof(date), "%s", v); }
                } else if (strncmp(line, "message", 7) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) { char *v = eq+1; while(*v==' ')v++;
                              snprintf(message, sizeof(message), "%s", v); }
                }
                p = nl ? nl + 1 : p + len;
                if (!nl) break;
            }
        }
        zip_close(za);

        // Mark archived commits.
        char isArchived = (strcmp(zips[z].dir, archDir) == 0) ? '*' : ' ';
        printf("  %-22s%c %-24s  %s\n",
               zips[z].name, isArchived, date, message);
        found++;
    }

    if (found == 0) {
        printf("  (file not found in any commit archive)\n");
        printf("  Has '%s' been committed? Run 'vc commit' first.\n", relPath);
    } else {
        printf("\n  %d commit(s) found containing '%s'.\n", found, relPath);
        if (zipCount > 0 &&
            strcmp(zips[0].dir, archDir) == 0)
            printf("  (* = archived commit)\n");
    }
    printf("\n");

    free(zips);
    return 0;
}
