#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include <pwd.h>
#include "vc.h"
#include "vcTransport.h"
#include "vcd.h"
#include "vcAuth.h"

// -------------------------------------------------------------------
// promptIfEmpty  –  if *value is NULL or empty, prompt the user.
// Uses readline so the user gets line editing.
// Returns false if the user provides nothing after prompting.
// -------------------------------------------------------------------
static bool promptIfEmpty(char **value, const char *prompt) {
    if (*value != NULL && (*value)[0] != '\0')
        return true;   // already supplied on the command line

    char *input = readline(prompt);
    if (input == NULL || input[0] == '\0') {
        free(input);
        return false;
    }

    free(*value);
    *value = input;    // readline allocates; we now own it
    return true;
}

// parseInitArgs  –  scan argv for --user, --email, --project, --repo.
//
// Accepted forms:
//   vc init --user "Jane Doe" --email jane@x.com --project myapp --repo /srv/vc
//   vc init --user=Jane (equals form also accepted)
// -------------------------------------------------------------------
static void parseInitArgs(int argc, char *argv[],
                          char **user, char **email,
                          char **project, char **repo,
                          char **host, char **loginName,
                          bool *isPrivate) {
    for (int i = 2; i < argc; i++) {
        char *arg = argv[i];

        // --repoPrivate is a boolean flag with no value required.
        if (strcmp(arg, "--repoPrivate") == 0) {
            *isPrivate = true;
            continue;
        }

        char *val = NULL;

        // Check for embedded '=' e.g. --user=Jane
        char *eq = strchr(arg, '=');
        if (eq != NULL) {
            *eq = '\0';
            val = eq + 1;
        } else if (i + 1 < argc && argv[i + 1][0] != '-') {
            // Next token is the value.
            val = argv[++i];
        }

        if (val == NULL || val[0] == '\0') {
            fprintf(stderr, "Warning: '%s' missing value – will prompt.\n", arg);
            if (eq) *eq = '=';
            continue;
        }

        if      (strcmp(arg, "--user")      == 0) *user      = val;
        else if (strcmp(arg, "--email")     == 0) *email     = val;
        else if (strcmp(arg, "--project")   == 0) *project   = val;
        else if (strcmp(arg, "--repoPath")  == 0) *repo      = val;
        else if (strcmp(arg, "--repoHost")  == 0) *host      = val;
        else if (strcmp(arg, "--repoLogin") == 0) *loginName = val;
        else
            fprintf(stderr, "Warning: unknown option '%s' ignored.\n", arg);

        if (eq) *eq = '=';  // restore argv
    }
}

// -------------------------------------------------------------------
// mkdirSafe  –  mkdir with a descriptive error on failure.
// -------------------------------------------------------------------
static bool mkdirSafe(const char *path) {
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "ERROR: Cannot create '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    return true;
}

// -------------------------------------------------------------------
// vcInit  –  initialise a new vc repository in the current directory.
//
// Usage:
//   vc init
//   vc init --user NAME --email EMAIL [--project NAME]
//           [--repoHost HOST] [--repoLogin NAME] [--repoPrivate]
//           [--repoPath PATH]
//
// Required: user, email.
// Project defaults to the current directory name.
// Remote settings (repoPath, repoHost) are optional — set later with
// vc config --set repo <path>.
// -------------------------------------------------------------------
int vcInit(int argc, char *argv[]) {

    printf("Initialising vc repository...\n\n");

    // 1. Parse command-line flags.
    char *argUser      = NULL;
    char *argEmail     = NULL;
    char *argProject   = NULL;
    char *argRepo      = NULL;
    char *argHost      = NULL;
    char *argLoginName = NULL;
    bool  isPrivate    = false;

    parseInitArgs(argc, argv, &argUser, &argEmail, &argProject,
                  &argRepo, &argHost, &argLoginName, &isPrivate);

    // 2. Copy flag values into owned heap strings so we can free
    //    them uniformly at cleanup regardless of their origin.
    char *user      = argUser      ? strdup(argUser)      : NULL;
    char *emailVal  = argEmail     ? strdup(argEmail)     : NULL;
    char *project   = argProject   ? strdup(argProject)   : NULL;
    char *repo      = argRepo      ? strdup(argRepo)      : NULL;
    char *host      = argHost      ? strdup(argHost)      : NULL;
    char *loginName = argLoginName ? strdup(argLoginName) : NULL;
    char  password[256] = "";   // vcd password — saved to .vc/auth after init

    // Default project name to the current directory name if not supplied.
    if (project == NULL) {
        char cwd[MAX_DIR_PATH];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            const char *base = strrchr(cwd, '/');
            if (base != NULL && *(base + 1) != '\0')
                project = strdup(base + 1);
        }
    }

    // 3. Default user name to OS login name if not supplied via --user.
    if (user == NULL) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_gecos && pw->pw_gecos[0] != '\0') {
            // pw_gecos often contains full name (e.g. "Kelly Wiles,,,")
            char gecos[256];
            snprintf(gecos, sizeof(gecos), "%s", pw->pw_gecos);
            // Strip trailing commas
            char *comma = strchr(gecos, ',');
            if (comma) *comma = '\0';
            if (gecos[0] != '\0') user = strdup(gecos);
        }
        if (user == NULL && pw && pw->pw_name)
            user = strdup(pw->pw_name);
    }

    // 4. Prompt for any values still missing.
    printf("Enter repository details (leave blank to cancel):\n\n");

    // User name — only prompt if we couldn't derive it from the OS.
    if (user != NULL && user[0] != '\0') {
        printf("  User name    : %s\n", user);
    } else {
        if (!promptIfEmpty(&user, "  User name    : ")) {
            fprintf(stderr, "\nERROR: User name is required. Aborting.\n");
            goto fail;
        }
    }
    if (!promptIfEmpty(&emailVal, "  Email        : ")) {
        fprintf(stderr, "\nERROR: Email is required. Aborting.\n");
        goto fail;
    }
    // Project name: use directory name (or --project flag), no prompt.
    if (project != NULL && project[0] != '\0') {
        printf("  Project name : %s\n", project);
    } else {
        fprintf(stderr, "\nERROR: Could not determine project name. "
                        "Use --project <name>.\n");
        goto fail;
    }
    // Host, vcdUser and loginName are optional — leave blank to skip.
    if (host == NULL) {
        char *input = readline("  Remote host   : (optional, Enter to skip) ");
        if (input != NULL && input[0] != '\0')
            host = input;
        else
            free(input);
    }

    // If host is set, prompt for vcdUser and auto-derive the repo path.
    char autoRepo[MAX_DIR_PATH] = "";
    if (host != NULL && host[0] != '\0') {
        // Prompt for vcd username.
        char *vcdUserInput = readline("  vcd username  : ");
        if (vcdUserInput != NULL && vcdUserInput[0] != '\0') {
            // Store as loginName so it gets written to config.vc.
            free(loginName);
            loginName = vcdUserInput;

            // Prompt for password and ask the server for the user directory.
            // During init there's no .vc dir yet, so prompt directly.
            printf("  vcd password  : ");
            fflush(stdout);
            vc_echo_off();
            if (fgets(password, sizeof(password), stdin) != NULL) {
                size_t pl = strlen(password);
                if (pl > 0 && password[pl-1] == '\n') password[--pl] = '\0';
                if (pl > 0 && password[pl-1] == '\r') password[--pl] = '\0';
            }
            vc_echo_on();
            printf("\n");

            // Connect to vcd to verify credentials are correct.
            // In the new protocol, the user's repo dir is always:
            //   users/<vcdUser>/<projectname>
            // so we can derive it locally without a GETDIR command.
            printf("  Verifying vcd credentials...\n");
            VcSession vs;
            int port = VCD_DEFAULT_PORT;
            if (!vct_connect(&vs, host, port, loginName, password)) {
                memset(password, 0, sizeof(password));
                fprintf(stderr, "\nERROR: Cannot connect to vcd at %s:%d\n"
                                "       Ensure vcd is running on the remote:\n"
                                "         sudo vcd --start\n"
                                "       Then re-run: vc init\n",
                        host, port);
                goto fail;
            }
            vct_disconnect(&vs);
            memset(password, 0, sizeof(password));
            printf("  Credentials verified.\n");

            // Build repo path: users/<vcdUser>/<projectname>
            snprintf(autoRepo, sizeof(autoRepo), "%s/%s/%s",
                     VCD_USERS_DIR, loginName, project);
            printf("  Repo path     : %s\n", autoRepo);
        } else {
            free(vcdUserInput);
            fprintf(stderr, "\nERROR: vcd username is required when a remote host is set.\n"
                            "       Either provide a username or leave host blank to skip remote setup.\n");
            goto fail;
        }
    }

    // Use auto-derived repo path if we got one, otherwise leave blank.
    if (repo == NULL && autoRepo[0] != '\0')
        repo = strdup(autoRepo);

    // Private setting — prompt only if not already set via --repoPrivate flag.
    if (!isPrivate) {
        char *input = readline("  Private repo  : restrict push/pull to this user? [y/N] ");
        if (input != NULL) {
            isPrivate = (input[0] == 'y' || input[0] == 'Y');
            free(input);
        }
    }

    // 4. Show a summary before touching the filesystem.
    printf("\nRepository will be created with:\n");
    printf("  User    : %s\n", user);
    printf("  Email   : %s\n", emailVal);
    printf("  Project : %s\n", project);
    if (host)
        printf("  Host    : %s\n", host);
    if (loginName)
        printf("  vcdUser : %s\n", loginName);
    if (repo && repo[0])
        printf("  Repo    : %s\n", repo);
    printf("  Private : %s\n", isPrivate ? "yes" : "no");
    printf("\n");

    // 5. Create directory structure using full paths – avoids the
    //    original chdir-then-lose-cwd bug.
    char cwd[MAX_DIR_PATH];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "ERROR: Cannot get current directory: %s\n",
                strerror(errno));
        goto fail;
    }

    char vcDir[MAX_DIR_PATH];
    char dataDir[MAX_DIR_PATH];
    char archiveDir[MAX_DIR_PATH];

    snprintf(vcDir,      sizeof(vcDir),      "%s/.vc",         cwd);
    snprintf(dataDir,    sizeof(dataDir),    "%s/.vc/data",    cwd);
    snprintf(archiveDir, sizeof(archiveDir), "%s/.vc/archive", cwd);

    if (!mkdirSafe(vcDir))      goto fail;
    if (!mkdirSafe(dataDir))    goto fail;
    if (!mkdirSafe(archiveDir)) goto fail;

    // Initialise the branch structure (creates .vc/branches/main and HEAD).
    if (vcBranchEnsureInit() != 0) goto fail;

    // Initialise the tags directory.
    if (vcTagEnsureInit() != 0) goto fail;

    // 6. Write config.vc.
    char cfgPath[MAX_DIR_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s/%s", cwd, VC_CFGFILE);

    FILE *f = fopen(cfgPath, "w");
    if (f == NULL) {
        fprintf(stderr, "ERROR: Cannot create '%s': %s\n",
                cfgPath, strerror(errno));
        goto fail;
    }

    fprintf(f, "[Repo]\n");
    fprintf(f, "  user      = %s\n", user);
    fprintf(f, "  email     = %s\n", emailVal);
    fprintf(f, "  project   = %s\n", project);
    if (repo && repo[0] != '\0')
        fprintf(f, "  repo      = %s\n", repo);
    if (host)
        fprintf(f, "  host      = %s\n", host);
    if (loginName)
        fprintf(f, "  vcdUser   = %s\n", loginName);

    // Record the OS login name of whoever ran 'vc init' as the owner.
    // This is used to enforce private repo access control.
    const char *osUser = getenv("USER");
    if (!osUser || osUser[0] == '\0') osUser = getenv("LOGNAME");
    if (!osUser || osUser[0] == '\0') osUser = "unknown";
    fprintf(f, "  owner     = %s\n", osUser);
    fprintf(f, "  private   = %s\n", isPrivate ? "true" : "false");
    fclose(f);

    // 7. Populate the global variables so any further operations in
    //    the same run (e.g. immediately running vc add) see them.
    free(userName);  userName  = strdup(user);
    free(email);     email     = strdup(emailVal);
    free(prjName);   prjName   = strdup(project);
    free(repoPath);  repoPath  = strdup(repo ? repo : "");
    if (host)      { free(hostName); hostName = strdup(host); }

    printf("Repository initialised in %s/.vc/\n", cwd);
    printf("Initial branch : main\n");

    // Save vcd password to .vc/auth now that the directory exists.
    if (host && loginName && password[0] != '\0') {
        if (vcAuthSave(cwd, host, loginName, password))
            printf("vcd password   : saved to .vc/auth (encrypted).\n");
        memset(password, 0, sizeof(password));
    }

    if (!host || !loginName)
        printf("Tip: run 'vc config' to add remote host and vcd user for push/pull.\n");
    else if (autoRepo[0])
        printf("Create the remote repo:  sudo vcd --initrepo %s\n", autoRepo);
    printf("Run 'vc add' to start tracking files.\n");

    free(user);
    free(emailVal);
    free(project);
    free(repo);
    free(host);
    free(loginName);
    return 0;

fail:
    free(user);
    free(emailVal);
    free(project);
    free(repo);
    free(host);
    free(loginName);
    return -1;
}
