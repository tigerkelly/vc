
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
// vcShow  –  display full details of a specific commit archive.
//
// Usage:
//   vc show                    Show the most recent commit
//   vc show <archive>          Show a specific zip by name
//   vc show --tag <name>       Show the commit a tag points to
// -------------------------------------------------------------------
int vcShow(int argc, char *argv[]) {

    vcLog("%s\n", __func__);

    char zipPath[MAX_DIR_PATH] = "";

    if (argc < 3) {
        // No argument — show the most recent commit (HEAD_COMMIT).
        char headCommit[MAX_DIR_PATH];
        char *branch = vcBranchCurrentName();
        snprintf(headCommit, sizeof(headCommit),
                 "%s/.vc/branches/%s/HEAD_COMMIT", vcTopDir, branch);
        free(branch);

        FILE *hf = fopen(headCommit, "r");
        if (hf == NULL) {
            fprintf(stderr, "vcShow: no commits found on this branch.\n"
                            "        Run 'vc commit' first.\n");
            return -1;
        }
        char zipName[64] = "";
        if (fgets(zipName, sizeof(zipName), hf)) {
            size_t l = strlen(zipName);
            if (l > 0 && zipName[l-1] == '\n') zipName[--l] = '\0';
        }
        fclose(hf);
        snprintf(zipPath, sizeof(zipPath), "%s/.vc/data/%s", vcTopDir, zipName);

    } else if (strcmp(argv[2], "--tag") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcShow: --tag requires a tag name.\n");
            return -1;
        }
        char tagFile[MAX_DIR_PATH];
        snprintf(tagFile, sizeof(tagFile), "%s/.vc/tags/%s", vcTopDir, argv[3]);
        FILE *tf = fopen(tagFile, "r");
        if (tf == NULL) {
            fprintf(stderr, "vcShow: tag '%s' not found.\n", argv[3]);
            return -1;
        }
        char line[256];
        while (fgets(line, sizeof(line), tf) != NULL) {
            if (strncmp(line, "commit", 6) == 0) {
                char *eq = strchr(line, '=');
                if (eq) {
                    char *v = eq + 1;
                    while (*v == ' ') v++;
                    size_t l = strlen(v);
                    if (l > 0 && v[l-1] == '\n') v[--l] = '\0';
                    snprintf(zipPath, sizeof(zipPath),
                             "%s/.vc/data/%s", vcTopDir, v);
                }
                break;
            }
        }
        fclose(tf);

    } else {
        // Bare name or full path.
        const char *arg = argv[2];
        if (strchr(arg, '/') != NULL) {
            snprintf(zipPath, sizeof(zipPath), "%s", arg);
        } else {
            // Try data/ first, then archive/.
            snprintf(zipPath, sizeof(zipPath), "%s/.vc/data/%s", vcTopDir, arg);
            struct stat st;
            if (stat(zipPath, &st) != 0)
                snprintf(zipPath, sizeof(zipPath), "%s/.vc/archive/%s",
                         vcTopDir, arg);
        }
    }

    // Verify the zip exists.
    struct stat st;
    if (stat(zipPath, &st) != 0) {
        fprintf(stderr, "vcShow: archive not found: %s\n", zipPath);
        return -1;
    }

    // Open the zip.
    int err = 0;
    zip_t *za = zip_open(zipPath, ZIP_RDONLY, &err);
    if (za == NULL) {
        fprintf(stderr, "vcShow: cannot open archive '%s'\n", zipPath);
        return -1;
    }

    // Extract and print MANIFEST.txt.
    const char *archiveName = strrchr(zipPath, '/');
    archiveName = archiveName ? archiveName + 1 : zipPath;

    printf("\nCommit : %s\n", archiveName);

    // Format size.
    char sizeStr[24];
    if (st.st_size >= 1024*1024)
        snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", (double)st.st_size/(1024*1024));
    else if (st.st_size >= 1024)
        snprintf(sizeStr, sizeof(sizeStr), "%.1f KB", (double)st.st_size/1024);
    else
        snprintf(sizeStr, sizeof(sizeStr), "%lld B", (long long)st.st_size);
    printf("Size   : %s\n\n", sizeStr);

    zip_file_t *zf = zip_fopen(za, "MANIFEST.txt", 0);
    if (zf) {
        char buf[2048] = {0};
        zip_fread(zf, buf, sizeof(buf)-1);
        zip_fclose(zf);
        printf("%s\n", buf);
    }

    // List all files in the archive.
    zip_int64_t n = zip_get_num_entries(za, 0);

    // Count non-manifest, non-dir entries for the header.
    int fileCount = 0;
    int dirCount  = 0;
    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (name == NULL || strcmp(name, "MANIFEST.txt") == 0) continue;
        size_t nlen = strlen(name);
        if (nlen > 0 && name[nlen-1] == '/') dirCount++;
        else fileCount++;
    }

    printf("Files in this commit (%d file%s%s):\n\n",
           fileCount, fileCount != 1 ? "s" : "",
           dirCount > 0 ? ", plus empty dirs" : "");

    printf("  %-38s  %8s  %8s  %5s  %s\n",
           "Path", "Size", "Stored", "Ratio", "Modified");
    printf("  %-38s  %8s  %8s  %5s  %s\n",
           "--------------------------------------",
           "--------", "--------", "-----",
           "-------------------");

    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (name == NULL || strcmp(name, "MANIFEST.txt") == 0) continue;

        zip_stat_t zst;
        zip_stat_index(za, i, 0, &zst);

        size_t nlen = strlen(name);
        bool isDir = (nlen > 0 && name[nlen-1] == '/');

        if (isDir) {
            printf("  %-38s  %8s  %8s  %5s\n",
                   name, "<dir>", "", "");
            continue;
        }

        // Uncompressed size.
        char szUncomp[16];
        if (zst.size >= 1024*1024)
            snprintf(szUncomp, sizeof(szUncomp), "%6.1fMB", (double)zst.size/(1024*1024));
        else if (zst.size >= 1024)
            snprintf(szUncomp, sizeof(szUncomp), "%6.1fKB", (double)zst.size/1024);
        else
            snprintf(szUncomp, sizeof(szUncomp), "%6lluB ", (unsigned long long)zst.size);

        // Compressed size.
        char szComp[16];
        if (zst.comp_size >= 1024*1024)
            snprintf(szComp, sizeof(szComp), "%6.1fMB", (double)zst.comp_size/(1024*1024));
        else if (zst.comp_size >= 1024)
            snprintf(szComp, sizeof(szComp), "%6.1fKB", (double)zst.comp_size/1024);
        else
            snprintf(szComp, sizeof(szComp), "%6lluB ", (unsigned long long)zst.comp_size);

        // Compression ratio.
        char ratio[8] = "  n/a";
        if (zst.size > 0) {
            int pct = (int)(100 - (zst.comp_size * 100 / zst.size));
            snprintf(ratio, sizeof(ratio), "%4d%%", pct);
        }

        // Modification time.
        char mtime[24] = "(unknown)";
        if (zst.mtime > 0) {
            struct tm *tm = localtime(&zst.mtime);
            strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M:%S", tm);
        }

        printf("  %-38s  %8s  %8s  %5s  %s\n",
               name, szUncomp, szComp, ratio, mtime);
    }
    printf("\n");

    zip_close(za);
    return 0;
}
