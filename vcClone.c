// vcClone.c  –  clone a remote repo from vcd daemon.
//
// Usage: vc clone <host> <repopath> [localdir]
// Example:
//   vc clone 192.168.0.170 users/kelly/ushell
//   vc clone 192.168.0.170 users/kelly/ushell /home/pi/work/ushell

#include <stdio.h>
#include "vcPlatform.h"
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
    VcSession  *s;
    const char *repoPath;
    const char *localBase;
    VcProgress *prog;
    int         pulled;
    int         failed;
} CloneCtx;

int vcClone(int argc, char *argv[]) {
    vcLog("%s\n", __func__);

    // Usage: vc clone [--user <username>] <host> <repopath> [localdir]
    // Parse optional --user flag.
    char flagUser[MAX_USERNAME] = "";
    int  argStart = 2;
    if (argc >= 4 && strcmp(argv[2], "--user") == 0) {
        snprintf(flagUser, sizeof(flagUser), "%s", argv[3]);
        argStart = 4;
    }

    if (argc < argStart + 2) {
        if (argc == 3 && (strchr(argv[2], '@') || strchr(argv[2], ':'))) {
            fprintf(stderr,
                    "vcClone: SSH-style syntax not supported.\n"
                    "         Use: vc clone [--user <name>] <host> <repopath>\n"
                    "         Example: vc clone 192.168.0.170 users/kelly/ushell\n");
            return -1;
        }
        fprintf(stderr,
                "Usage: vc clone [--user <username>] <host> <repopath> [localdir]\n"
                "Example:\n"
                "  vc clone 192.168.0.170 users/kelly/ushell\n"
                "  vc clone --user kelly 192.168.0.170 users/kelly/ushell\n"
                "  vc clone 192.168.0.170 shared/teamapp /home/pi/teamapp\n");
        return -1;
    }

    const char *host     = argv[argStart];
    const char *repoPath = argv[argStart + 1];

    // Default local dir = last component of repoPath.
    const char *lastSlash = strrchr(repoPath, '/');
    const char *localDir  = (argc > argStart + 2) ? argv[argStart + 2]
                                                   : (lastSlash ? lastSlash + 1 : repoPath);

    // Sanity check repoPath starts with users/ or shared/.
    if (strncmp(repoPath, "users/",  6) != 0 &&
        strncmp(repoPath, "shared/", 7) != 0) {
        fprintf(stderr, "vcClone: repopath must start with 'users/' or 'shared/'\n"
                        "         Example: users/kelly/ushell\n");
        return -1;
    }

    // Get vcdUser: --user flag > config > prompt.
    extern Config *config;
    int  port    = VCD_DEFAULT_PORT;
    char vcdUser[MAX_USERNAME] = "";

    if (flagUser[0]) {
        snprintf(vcdUser, sizeof(vcdUser), "%s", flagUser);
    } else if (config) {
        if (config->port > 0) port = config->port;
        if (config->vcdUser[0])
            snprintf(vcdUser, sizeof(vcdUser), "%s", config->vcdUser);
        else if (config->loginName[0])
            snprintf(vcdUser, sizeof(vcdUser), "%s", config->loginName);
    }

    if (vcdUser[0] == '\0') {
        printf("vcd username: ");
        fflush(stdout);
        if (fgets(vcdUser, sizeof(vcdUser), stdin)) {
            size_t l = strlen(vcdUser);
            if (l > 0 && vcdUser[l-1] == '\n') vcdUser[--l] = '\0';
        }
        if (vcdUser[0] == '\0') {
            fprintf(stderr, "vcClone: vcd username required.\n"
                            "         Run: vc config --set vcdUser <username>\n");
            return -1;
        }
    }

    // During clone there is no .vc/auth yet — prompt directly.
    char password[256] = "";
    printf("vcd password for %s@%s: ", vcdUser, host);
    fflush(stdout);
    vc_echo_off();
    if (fgets(password, sizeof(password), stdin) != NULL) {
        size_t pl = strlen(password);
        if (pl > 0 && password[pl-1] == '\n') password[--pl] = '\0';
    }
    vc_echo_on();
    printf("\n");
    if (password[0] == '\0') {
        fprintf(stderr, "vcClone: no password provided.\n");
        return -1;
    }

    // Check destination doesn't exist.
    struct stat st;
    if (stat(localDir, &st) == 0) {
        fprintf(stderr, "vcClone: destination '%s' already exists.\n", localDir);
        memset(password, 0, sizeof(password));
        return -1;
    }

    printf("Connecting to vcd at %s:%d ...\n", host, port);

    VcSession s;
    if (!vct_connect(&s, host, port, vcdUser, password)) {
        memset(password, 0, sizeof(password));
        return -1;
    }
    // Password saved after clone; clear from memory on failure paths.
    memset(password, 0, sizeof(password));

    printf("Cloning %s:%s → %s\n", host, repoPath, localDir);

    // Create destination directory.
    if (mkdir(localDir, 0755) != 0) {
        fprintf(stderr, "vcClone: cannot create '%s': %s\n",
                localDir, strerror(errno));
        vct_disconnect(&s);
        return -1;
    }

    // Collect the full file list BEFORE issuing any PULL commands.
    // LIST and PULL share the same TCP connection — the LIST response
    // must be fully drained before any PULL command is sent.
    VctFileEntry *files = NULL;
    int total = vct_list_collect(&s, repoPath, &files);
    if (total < 0) {
        fprintf(stderr, "vcClone: LIST failed\n");
        vct_disconnect(&s);
        return -1;
    }

    VcProgress prog;
    vcp_init(&prog, total);

    int pulled = 0, failed = 0;
    for (int i = 0; i < total; i++) {
        char localPath[MAX_DIR_PATH];
        snprintf(localPath, sizeof(localPath), "%s/%s",
                 localDir, files[i].relPath);
        if (vct_pull(&s, repoPath, files[i].relPath, localPath, false)) {
            vcp_update(&prog, "clone", files[i].relPath);
            pulled++;
        } else {
            vcp_fail(&prog, files[i].relPath);
            failed++;
        }
    }
    free(files);

    vcp_done(&prog);
    vct_disconnect(&s);

    if (failed > 0) {
        fprintf(stderr, "vcClone: %d file(s) failed. Clone may be incomplete.\n",
                failed);
        return -1;
    }

    printf("Clone complete: %d files.\n", pulled);
    if (vcAuthSave(localDir, host, vcdUser, password))
        printf("Password saved to %s/.vc/auth\n", localDir);
    memset(password, 0, sizeof(password));

    // Checkout the latest commit so working files are present.
    // Must chdir into the cloned repo first since vcCheckout and
    // vcBranch use relative paths anchored to the current directory.
    char absLocalDir[MAX_DIR_PATH];
    if (realpath(localDir, absLocalDir) == NULL)
        snprintf(absLocalDir, sizeof(absLocalDir), "%s", localDir);

    char savedCwd[MAX_DIR_PATH];
    if (getcwd(savedCwd, sizeof(savedCwd)) == NULL) savedCwd[0] = '\0';

    extern char *vcTopDir;
    char *savedTopDir = vcTopDir;
    vcTopDir = absLocalDir;

    if (chdir(absLocalDir) == 0) {
        printf("Checking out latest commit...\n");
        char *coArgv[] = { "vc", "checkout", "--all", NULL };
        int   coRc     = vcCheckout(3, coArgv);
        if (coRc != 0)
            fprintf(stderr, "vcClone: checkout failed — run 'vc checkout --all'"
                            " from inside the repo.\n");
        if (savedCwd[0]) chdir(savedCwd);
    } else {
        fprintf(stderr, "vcClone: cannot chdir to '%s': %s\n",
                absLocalDir, strerror(errno));
    }

    vcTopDir = savedTopDir;

    printf("Enter the repo: cd %s\n", localDir);
    return 0;
}
