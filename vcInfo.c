
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <zip.h>

#include "vc.h"

// -------------------------------------------------------------------
// vcInfo  –  show a repository summary.
//
// Displays:
//   Project, branch, remote host, last commit, commit count,
//   branch count, tag count, staged/modified/untracked file counts,
//   and repo size on disk.
// -------------------------------------------------------------------
int vcInfo(void) {

    vcLog("%s %s\n", __func__, vcTopDir);

    extern Config *config;

    // --- Project and branch ---
    char branch[128] = "main";
    char headPath[MAX_DIR_PATH];
    snprintf(headPath, sizeof(headPath), "%s/.vc/HEAD", vcTopDir);
    FILE *hf = fopen(headPath, "r");
    if (hf) {
        if (fgets(branch, sizeof(branch), hf)) {
            size_t l = strlen(branch);
            if (l > 0 && branch[l-1] == '\n') branch[--l] = '\0';
        }
        fclose(hf);
    }

    const char *project = (config && config->project[0])
                          ? config->project : "(unknown)";
    const char *host    = (config && config->host[0])
                          ? config->host : "(not configured)";
    const char *login   = (config && config->loginName[0])
                          ? config->loginName : "";
    const char *repo    = (config && config->repo[0])
                          ? config->repo : "(not configured)";

    // --- Count commits in .vc/data/ ---
    char dataDir[MAX_DIR_PATH];
    snprintf(dataDir, sizeof(dataDir), "%s/.vc/data", vcTopDir);
    int commitCount = 0;
    long repoBytes  = 0;
    char newestZip[256] = "";
    DIR *dir = opendir(dataDir);
    if (dir) {
        struct dirent *de;
        struct stat st;
        char zipPath[MAX_DIR_PATH];
        while ((de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0) {
                commitCount++;
                snprintf(zipPath, sizeof(zipPath), "%s/%s", dataDir, de->d_name);
                if (stat(zipPath, &st) == 0) repoBytes += st.st_size;
                if (strcmp(de->d_name, newestZip) > 0)
                    snprintf(newestZip, 256, "%s", de->d_name);
            }
        }
        closedir(dir);
    }

    // --- Count archived commits ---
    char archDir[MAX_DIR_PATH];
    snprintf(archDir, sizeof(archDir), "%s/.vc/archive", vcTopDir);
    int archiveCount = 0;
    dir = opendir(archDir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len > 4 && strcmp(de->d_name + len - 4, ".zip") == 0)
                archiveCount++;
        }
        closedir(dir);
    }

    // --- Count branches ---
    char branchDir[MAX_DIR_PATH];
    snprintf(branchDir, sizeof(branchDir), "%s/.vc/branches", vcTopDir);
    int branchCount = 0;
    dir = opendir(branchDir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
            if (de->d_name[0] != '.') branchCount++;
        closedir(dir);
    }

    // --- Count tags ---
    char tagDir[MAX_DIR_PATH];
    snprintf(tagDir, sizeof(tagDir), "%s/.vc/tags", vcTopDir);
    int tagCount = 0;
    dir = opendir(tagDir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
            if (de->d_name[0] != '.') tagCount++;
        closedir(dir);
    }

    // --- Read last commit message from newest zip manifest ---
    char lastMsg[128]  = "(no commits yet)";
    char lastDate[32]  = "";
    char lastAuthor[64] = "";
    if (newestZip[0]) {
        char zipPath[MAX_DIR_PATH];
        snprintf(zipPath, sizeof(zipPath), "%s/%s", dataDir, newestZip);
        int err = 0;
        zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
        if (za) {
            zip_file_t *zf = zip_fopen(za, "MANIFEST.txt", 0);
            if (zf) {
                char buf[2048] = {0};
                zip_fread(zf, buf, sizeof(buf)-1);
                zip_fclose(zf);
                // Parse fields.
                for (const char *p = buf; *p; ) {
                    char line[2048];
                    const char *nl = strchr(p, '\n');
                    size_t len = nl ? (size_t)(nl - p) : strlen(p);
                    if (len >= sizeof(line)) len = sizeof(line)-1;
                    snprintf(line, len+1, "%s", p);
                    if (strncmp(line, "message", 7) == 0) {
                        char *eq = strchr(line, '=');
                        if (eq) { char *v = eq+1; while(*v==' ')v++;
                                  snprintf(lastMsg, sizeof(lastMsg), "%s", v); }
                    } else if (strncmp(line, "date", 4) == 0) {
                        char *eq = strchr(line, '=');
                        if (eq) { char *v = eq+1; while(*v==' ')v++;
                                  snprintf(lastDate, sizeof(lastDate), "%s", v); }
                    } else if (strncmp(line, "author", 6) == 0) {
                        char *eq = strchr(line, '=');
                        if (eq) { char *v = eq+1; while(*v==' ')v++;
                                  snprintf(lastAuthor, sizeof(lastAuthor), "%s", v); }
                    }
                    p = nl ? nl + 1 : p + len;
                    if (!nl) break;
                }
            }
            zip_close(za);
        }
    }

    // --- Count index states ---
    IndexEntry *entries = NULL;
    int idxCount = vcIndexLoad(&entries);
    int staged = 0, modified = 0, committed = 0;
    for (int i = 0; i < idxCount; i++) {
        switch (entries[i].state) {
        case INDEX_STAGED:    staged++;    break;
        case INDEX_MODIFIED:  modified++;  break;
        case INDEX_COMMITTED: committed++; break;
        }
    }
    vcIndexFree(entries, idxCount);

    // --- Format repo size ---
    char sizeStr[24];
    if (repoBytes >= 1024*1024)
        snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", (double)repoBytes/(1024*1024));
    else if (repoBytes >= 1024)
        snprintf(sizeStr, sizeof(sizeStr), "%.1f KB", (double)repoBytes/1024);
    else
        snprintf(sizeStr, sizeof(sizeStr), "%ld B", repoBytes);

    // --- Print ---
    printf("\n");
    printf("  Repository   : %s\n", vcTopDir);
    printf("  Project      : %s\n", project);
    printf("  Branch       : %s\n", branch);
    printf("\n");
    printf("  Remote host  : %s\n", host);
    if (login[0])
        printf("  Login        : %s\n", login);
    printf("  Remote path  : %s\n", repo);
    printf("\n");
    printf("  Commits      : %d active", commitCount);
    if (archiveCount > 0)
        printf(", %d archived", archiveCount);
    printf("\n");
    printf("  Branches     : %d\n", branchCount);
    printf("  Tags         : %d\n", tagCount);
    printf("  Repo size    : %s\n", sizeStr);
    printf("\n");
    if (newestZip[0]) {
        printf("  Last commit  : %s\n", newestZip);
        if (lastDate[0])
            printf("  Date         : %s\n", lastDate);
        if (lastAuthor[0])
            printf("  Author       : %s\n", lastAuthor);
        printf("  Message      : %s\n", lastMsg);
        printf("\n");
    }
    if (staged > 0 || modified > 0) {
        printf("  Working tree : ");
        if (staged > 0)   printf("%d staged  ", staged);
        if (modified > 0) printf("%d modified", modified);
        printf("\n\n");
    } else {
        printf("  Working tree : clean\n\n");
    }

    return 0;
}
