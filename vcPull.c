// vcPull.c  –  pull remote repo from vcd daemon.
//
// Uses LIST to get the full remote file tree, then pulls each file.
// Commit zips are skipped if already present at the same size.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#include "vc.h"
#include "vcd.h"
#include "vcTransport.h"
#include "vcAuth.h"
#include "vcProgress.h"

typedef struct {
    VcSession   *s;
    const char  *repoPath;
    VcProgress  *prog;
    int          pulled;
    int          skipped;
    int          failed;
} PullCtx;

int vcPull(int argc, char *argv[]) {
    vcLog("%s %s\n", __func__, vcTopDir);

    for (int i = 2; i < argc; i++) {
        fprintf(stderr, "vcPull: unknown option '%s'\n"
                        "        Usage: vc pull\n", argv[i]);
        return -1;
    }

    extern Config *config;
    if (!config || config->host[0] == '\0') {
        fprintf(stderr, "vcPull: no remote host configured.\n"
                        "        Run: vc config --set host <hostname>\n");
        return -1;
    }
    if (config->repo[0] == '\0') {
        fprintf(stderr, "vcPull: no remote repo path configured.\n"
                        "        Run: vc config --set repo users/<username>/<reponame>\n");
        return -1;
    }

    const char *host    = config->host;
    const char *vcdUser = config->vcdUser;
    int         port    = config->port > 0 ? config->port : VCD_DEFAULT_PORT;

    if (vcdUser[0] == '\0') {
        fprintf(stderr, "vcPull: no vcd username configured.\n"
                        "        Run: vc config --set vcdUser <username>\n");
        return -1;
    }

    char password[256] = "";
    if (!vcAuthPrompt(vcTopDir, host, vcdUser, password, sizeof(password))) {
        fprintf(stderr, "vcPull: no password provided.\n");
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
    printf("Pulling from %s:%s\n\n", host, repoPath);

    // Ensure local .vc/ structure exists.
    const char *subdirs[] = {
        "/.vc", "/.vc/data", "/.vc/branches", "/.vc/tags", "/.vc/stash", NULL
    };
    for (int i = 0; subdirs[i]; i++) {
        char path[MAX_DIR_PATH];
        snprintf(path, sizeof(path), "%s%s", vcTopDir, subdirs[i]);
        mkdir(path, 0755);
    }

    printf("Pulling from %s:%s\n", host, repoPath);

    // Collect the full file list BEFORE issuing any PULL commands.
    // LIST and PULL share the same TCP connection — the LIST response
    // must be fully drained before any PULL command is sent.
    VctFileEntry *files = NULL;
    int total = vct_list_collect(&s, repoPath, &files);
    if (total < 0) {
        fprintf(stderr, "vcPull: LIST failed — check vcd is running on %s:%d\n",
                host, port);
        vct_disconnect(&s);
        return -1;
    }

    VcProgress prog;
    vcp_init(&prog, total);

    int pulled = 0, skipped = 0, failed = 0;
    for (int i = 0; i < total; i++) {
        char localPath[MAX_DIR_PATH];
        snprintf(localPath, sizeof(localPath), "%s/%s", vcTopDir, files[i].relPath);
        // Commit zips are immutable — skip if already at correct size.
        size_t rl = strlen(files[i].relPath);
        bool isZip = rl > 4 &&
                     strcmp(files[i].relPath + rl - 4, ".zip") == 0;
        if (vct_pull(&s, repoPath, files[i].relPath, localPath, isZip)) {
            vcp_update(&prog, "pull", files[i].relPath);
            pulled++;
        } else {
            // Check if it was skipped (same-size zip).
            struct stat lst;
            if (isZip && stat(localPath, &lst) == 0 &&
                lst.st_size == files[i].size) {
                skipped++;
            } else {
                vcp_fail(&prog, files[i].relPath);
                failed++;
            }
        }
    }
    free(files);

    vcp_done(&prog);
    vct_disconnect(&s);
    printf("Pull complete: %d pulled, %d skipped, %d failed.\n",
           pulled, skipped, failed);
    return failed > 0 ? -1 : 0;
}
