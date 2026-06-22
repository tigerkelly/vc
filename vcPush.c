// vcPush.c  –  push local repo to remote vcd daemon.
//
// Transport: vcd daemon only (no SSH/SFTP).
// Credentials: loaded from .vc/auth or prompted.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include "vc.h"
#include "vcd.h"
#include "vcTransport.h"
#include "vcAuth.h"
#include "vcProgress.h"

// Count regular files under a directory tree recursively.
static int count_files(const char *base) {
    int count = 0;
    DIR *dir = opendir(base);
    if (!dir) return 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[MAX_DIR_PATH];
        snprintf(path, sizeof(path), "%s/%s", base, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))      count += count_files(path);
        else if (S_ISREG(st.st_mode)) count++;
    }
    closedir(dir);
    return count;
}

// Push all files under a local directory tree to the server.
static int push_dir(VcSession *s, const char *repoPath,
                    const char *localBase, const char *relPrefix,
                    VcProgress *prog, int *pushed, int *failed) {
    DIR *dir = opendir(localBase);
    if (!dir) return 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char localPath[MAX_DIR_PATH];
        snprintf(localPath, sizeof(localPath), "%s/%s", localBase, de->d_name);

        char relPath[MAX_DIR_PATH];
        snprintf(relPath, sizeof(relPath), "%s/%s", relPrefix, de->d_name);

        struct stat st;
        if (stat(localPath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            push_dir(s, repoPath, localPath, relPath, prog, pushed, failed);
        } else if (S_ISREG(st.st_mode)) {
            if (vct_push(s, repoPath, localPath, relPath)) {
                vcp_update(prog, "push", relPath);
                (*pushed)++;
            } else {
                vcp_fail(prog, relPath);
                (*failed)++;
            }
        }
    }
    closedir(dir);
    return 0;
}

int vcPush(int argc, char *argv[]) {
    vcLog("%s %s\n", __func__, vcTopDir);

    for (int i = 2; i < argc; i++) {
        fprintf(stderr, "vcPush: unknown option '%s'\n"
                        "        Usage: vc push\n", argv[i]);
        return -1;
    }

    extern Config *config;
    if (!config || config->host[0] == '\0') {
        fprintf(stderr, "vcPush: no remote host configured.\n"
                        "        Run: vc config --set host <hostname>\n");
        return -1;
    }
    if (config->repo[0] == '\0') {
        fprintf(stderr, "vcPush: no remote repo path configured.\n"
                        "        Run: vc config --set repo users/<username>/<reponame>\n");
        return -1;
    }

    const char *host    = config->host;
    const char *vcdUser = config->vcdUser[0] ? config->vcdUser : config->loginName;
    int         port    = config->port > 0 ? config->port : VCD_DEFAULT_PORT;

    if (vcdUser[0] == '\0') {
        fprintf(stderr, "vcPush: no vcd username configured.\n"
                        "        Run: vc config --set vcdUser <username>\n");
        return -1;
    }

    // Load cached password or prompt.
    char password[256] = "";
    if (!vcAuthPrompt(vcTopDir, host, vcdUser, password, sizeof(password))) {
        fprintf(stderr, "vcPush: no password provided.\n");
        return -1;
    }

    printf("Transport: vcd daemon (%s:%d)\n", host, port);

    VcSession s;
    if (!vct_connect(&s, host, port, vcdUser, password)) {
        memset(password, 0, sizeof(password));
        return -1;
    }
    memset(password, 0, sizeof(password));

    const char *repoPath = config->repo;
    printf("Pushing to %s:%s\n\n", host, repoPath);

    // Check if the remote repo exists silently.
    // If not, warn and offer to create it before proceeding.
    if (!vct_repo_exists(&s, repoPath)) {
        printf("WARNING: Remote repo does not exist: %s\n\n", repoPath);
        printf("Create it now on the server? [y/N] ");
        fflush(stdout);
        char ans[8] = "";
        if (fgets(ans, sizeof(ans), stdin) &&
            (ans[0] == 'y' || ans[0] == 'Y')) {
            if (!vct_initrepo(&s, repoPath)) {
                fprintf(stderr, "vcPush: failed to create remote repo.\n"
                                "        Ask your server admin to run:\n"
                                "          sudo vcd --initrepo %s\n",
                        repoPath);
                vct_disconnect(&s);
                return -1;
            }
            printf("Remote repo created: %s\n\n", repoPath);
        } else {
            printf("Push cancelled.\n");
            vct_disconnect(&s);
            return -1;
        }
    }

    int pushed = 0, failed = 0;

    // Count files first so we can show accurate percentage.
    char vcDir[MAX_DIR_PATH];
    snprintf(vcDir, sizeof(vcDir), "%s/.vc", vcTopDir);
    int total = count_files(vcDir);

    printf("Pushing %d file%s...\n", total, total == 1 ? "" : "s");

    VcProgress prog;
    vcp_init(&prog, total);

    push_dir(&s, repoPath, vcDir, ".vc", &prog, &pushed, &failed);

    vcp_done(&prog);
    vct_disconnect(&s);

    printf("Push complete: %d pushed, %d failed.\n", pushed, failed);

    if (failed > 0) {
        fprintf(stderr, "vcPush: %d file(s) failed to push.\n", failed);
        return -1;
    }
    return 0;
}
