#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "vc.h"
#include "vcd.h"

// -------------------------------------------------------------------
// promptField  –  show current value in brackets and prompt for a
// new one.  Press Enter to keep the existing value.
// -------------------------------------------------------------------
static void promptField(char *dest, size_t maxLen,
                         const char *label, bool *changed) {
    char prompt[256];

    if (dest[0] != '\0')
        snprintf(prompt, sizeof(prompt), "  %-14s [%s]: ", label, dest);
    else
        snprintf(prompt, sizeof(prompt), "  %-14s : ", label);

    char *input = readline(prompt);
    if (input == NULL) return;

    trim(input);

    if (input[0] != '\0') {
        strncpy(dest, input, maxLen - 1);
        dest[maxLen - 1] = '\0';
        *changed = true;
    }
    free(input);
}

// -------------------------------------------------------------------
// writeConfig  –  write all config fields back to config.vc.
// Also exported as vcConfigSave() for external callers.
// Preserves the [Repo] section header format used by vcInit.
// Omits host line if not set so the file stays clean.
// -------------------------------------------------------------------
static int writeConfig(Config *cfg) {
    FILE *f = fopen(VC_CFGFILE, "w");
    if (f == NULL) {
        fprintf(stderr, "vcConfig: cannot write '%s': %s\n",
                VC_CFGFILE, strerror(errno));
        return -1;
    }

    fprintf(f, "[Repo]\n");
    fprintf(f, "  user      = %s\n", cfg->user);
    fprintf(f, "  email     = %s\n", cfg->email);
    fprintf(f, "  project   = %s\n", cfg->project);
    fprintf(f, "  repo      = %s\n", cfg->repo);
    if (cfg->host[0] != '\0')
        fprintf(f, "  host         = %s\n", cfg->host);
    if (cfg->vcdUser[0] != '\0')
        fprintf(f, "  vcdUser      = %s\n", cfg->vcdUser);
    if (cfg->port > 0)
        fprintf(f, "  port         = %d\n", cfg->port);
    if (cfg->owner[0] != '\0')
        fprintf(f, "  owner        = %s\n", cfg->owner);
    fprintf(f, "  private      = %s\n", cfg->isPrivate ? "true" : "false");
    if (cfg->allowedUsers[0] != '\0')
        fprintf(f, "  allowedUsers = %s\n", cfg->allowedUsers);

    fclose(f);
    return 0;
}

// -------------------------------------------------------------------
// printConfig  –  display all current configuration values.
// -------------------------------------------------------------------
static void printConfig(Config *cfg) {
    printf("Current configuration:\n");
    printf("  %-14s : %s\n",  "User name",  cfg->user);
    printf("  %-14s : %s\n",  "Email",      cfg->email);
    printf("  %-14s : %s\n",  "Project",    cfg->project);
    printf("  %-14s : %s\n",  "Repo path",  cfg->repo[0]   ? cfg->repo      : "(not set)");
    printf("  %-14s : %s\n",  "Remote host", cfg->host[0]  ? cfg->host      : "(not set)");
    printf("  %-14s : %s\n",  "vcd user",   cfg->vcdUser[0] ? cfg->vcdUser  : "(not set)");
    if (cfg->port > 0)
        printf("  %-14s : %d\n", "vcd port",   cfg->port);
    else
        printf("  %-14s : %d (default)\n", "vcd port", VCD_DEFAULT_PORT);
    printf("  %-14s : %s\n",  "Owner",      cfg->owner[0]  ? cfg->owner     : "(not set)");
    printf("  %-14s : %s\n",  "Private",    cfg->isPrivate ? "yes"           : "no");
    if (cfg->allowedUsers[0] != '\0')
        printf("  %-14s : %s\n", "Allowed users", cfg->allowedUsers);
    else if (cfg->isPrivate)
        printf("  %-14s : (none — only owner has access)\n", "Allowed users");
    printf("\n");
}

// -------------------------------------------------------------------
// vcConfig  –  interactively view and update repository configuration.
// -------------------------------------------------------------------
int vcConfig(Config *cfg) {

    vcLog("%s %s\n", __func__, vcTopDir);

    printf("\nRepository configuration  (%s)\n", VC_CFGFILE);
    printf("Press Enter to keep the current value, or type a new one.\n");
    printf("Leave a field blank and press Enter to keep it unchanged.\n\n");

    bool changed = false;

    promptField(cfg->user,      sizeof(cfg->user),      "User name",    &changed);
    promptField(cfg->email,     sizeof(cfg->email),     "Email",        &changed);
    promptField(cfg->project,   sizeof(cfg->project),   "Project",      &changed);
    promptField(cfg->repo,      sizeof(cfg->repo),      "Repo path",    &changed);
    promptField(cfg->host,      sizeof(cfg->host),      "Remote host",  &changed);

    // Private setting — y/n prompt.
    {
        char privBuf[8];
        printf("  Private repo [%s]: ", cfg->isPrivate ? "yes" : "no");
        fflush(stdout);
        if (fgets(privBuf, sizeof(privBuf), stdin) != NULL) {
            size_t l = strlen(privBuf);
            if (l > 0 && privBuf[l-1] == '\n') privBuf[--l] = '\0';
            if (l > 0) {
                bool newVal = (privBuf[0] == 'y' || privBuf[0] == 'Y' ||
                               strcasecmp(privBuf, "true") == 0 ||
                               strcmp(privBuf, "1") == 0);
                if (newVal != cfg->isPrivate) {
                    cfg->isPrivate = newVal;
                    changed = true;
                }
            }
        }
    }

    printf("\n");

    if (changed) {
        if (writeConfig(cfg) == 0) {
            printf("Configuration saved.\n\n");
            printConfig(cfg);
        }
    } else {
        printf("No changes made.\n\n");
        printConfig(cfg);
    }

    return 0;
}

// -------------------------------------------------------------------
// vcConfigShow  –  display current config without prompting.
// Called when user runs:  vc config --show
// -------------------------------------------------------------------
int vcConfigShow(Config *cfg) {
    vcLog("%s %s\n", __func__, vcTopDir);
    printf("\n");
    printConfig(cfg);
    return 0;
}

// -------------------------------------------------------------------
// vcConfigSet  –  non-interactively set a single config value.
// Called when user runs:  vc config --set <key> <value>
// Useful for scripting.
// -------------------------------------------------------------------
int vcConfigSet(Config *cfg, const char *key, const char *value) {
    vcLog("%s key=%s\n", __func__, key);

    if (strcasecmp(key, "user") == 0)
        snprintf(cfg->user,      sizeof(cfg->user),      "%s", value);
    else if (strcasecmp(key, "email") == 0)
        snprintf(cfg->email,     sizeof(cfg->email),     "%s", value);
    else if (strcasecmp(key, "project") == 0)
        snprintf(cfg->project,   sizeof(cfg->project),   "%s", value);
    else if (strcasecmp(key, "repo") == 0)
        snprintf(cfg->repo,      sizeof(cfg->repo),      "%s", value);
    else if (strcasecmp(key, "host") == 0)
        snprintf(cfg->host,      sizeof(cfg->host),      "%s", value);
    else if (strcasecmp(key, "loginName") == 0) {
        // Migrate legacy loginName to vcdUser.
        snprintf(cfg->loginName, sizeof(cfg->loginName), "%s", value);
        if (cfg->vcdUser[0] == '\0')
            snprintf(cfg->vcdUser, sizeof(cfg->vcdUser), "%s", value);
    }
    else if (strcasecmp(key, "vcdUser") == 0)
        snprintf(cfg->vcdUser,      sizeof(cfg->vcdUser),      "%s", value);
    else if (strcasecmp(key, "port") == 0)
        cfg->port = atoi(value);
    else if (strcasecmp(key, "owner") == 0)
        snprintf(cfg->owner,        sizeof(cfg->owner),        "%s", value);
    else if (strcasecmp(key, "allowedUsers") == 0)
        snprintf(cfg->allowedUsers, sizeof(cfg->allowedUsers), "%s", value);
    else if (strcasecmp(key, "private") == 0) {
        if (strcasecmp(value, "true")  == 0 ||
            strcasecmp(value, "yes")   == 0 ||
            strcmp(value, "1") == 0) {
            cfg->isPrivate = true;
        } else if (strcasecmp(value, "false") == 0 ||
                   strcasecmp(value, "no")    == 0 ||
                   strcmp(value, "0") == 0) {
            cfg->isPrivate = false;
        } else {
            fprintf(stderr, "vcConfig: invalid value '%s' for 'private'.\n"
                            "          Use: true, false, yes, no, 1, or 0\n",
                    value);
            return -1;
        }
    }
    else {
        fprintf(stderr, "vcConfig: unknown key '%s'.\n"
                        "          Valid keys: user, email, project, repo, host,\n"
                        "                      vcdUser, port, owner,\n"
                        "                      private, allowedUsers\n",
                key);
        return -1;
    }

    if (writeConfig(cfg) == 0) {
        printf("Set %s = %s\n", key, value);
        return 0;
    }
    return -1;
}

int vcConfigSave(Config *cfg) { return writeConfig(cfg); }
