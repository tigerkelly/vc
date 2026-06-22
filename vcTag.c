
#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

#include "vc.h"

// -------------------------------------------------------------------
// Tag layout inside .vc/
//
//   .vc/tags/                 – one file per tag
//   .vc/tags/<tagname>        – tag metadata file
//
// Each tag file contains:
//   tag     = <tagname>
//   commit  = <zip filename>  (e.g. 20250513_142300.zip)
//   branch  = <branch name>
//   author  = <user name>
//   date    = <timestamp>
//   message = <optional message>
//
// Tags are lightweight pointers to a commit zip.  They are not
// branch-specific — all tags are visible regardless of active branch.
// -------------------------------------------------------------------

#define VC_TAGS_DIR  ".vc/tags"
#define MAX_TAG_NAME 128

// -------------------------------------------------------------------
// isValidTagName  –  reject names with path separators, spaces,
// or a leading dash.
// -------------------------------------------------------------------
static bool isValidTagName(const char *name) {
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '-') return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ' ' || *p == '\t')
            return false;
    }
    return true;
}

// -------------------------------------------------------------------
// tagExists  –  true if the tag file already exists.
// -------------------------------------------------------------------
static bool tagExists(const char *name) {
    char path[MAX_DIR_PATH];
    snprintf(path, sizeof(path), "%s/%s", VC_TAGS_DIR, name);
    return (access(path, F_OK) == 0);
}

// -------------------------------------------------------------------
// ensureTagsDir  –  create .vc/tags/ if it doesn't exist.
// -------------------------------------------------------------------
static bool ensureTagsDir(void) {
    struct stat st;
    if (stat(VC_TAGS_DIR, &st) == 0) return true;
    if (mkdir(VC_TAGS_DIR, 0755) != 0) {
        fprintf(stderr, "vcTag: cannot create '%s': %s\n",
                VC_TAGS_DIR, strerror(errno));
        return false;
    }
    return true;
}

// -------------------------------------------------------------------
// readTagField  –  read a named field from a tag file.
// Fills buf with the value, returns true on success.
// -------------------------------------------------------------------
static bool readTagField(const char *tagPath, const char *field,
                          char *buf, size_t sz) {
    FILE *f = fopen(tagPath, "r");
    if (f == NULL) return false;

    char line[512];
    bool found = false;
    size_t flen = strlen(field);

    while (fgets(line, sizeof(line), f) != NULL) {
        // Match "field = value" or "field=value" (spaces around = optional).
        if (strncmp(line, field, flen) != 0) continue;

        // Skip whitespace after the field name.
        const char *p = line + flen;
        while (*p == ' ' || *p == '\t') p++;

        // Expect '='.
        if (*p != '=') continue;
        p++;   // skip '='

        // Skip whitespace after '='.
        while (*p == ' ' || *p == '\t') p++;

        // Strip trailing newline.
        size_t vlen = strlen(p);
        if (vlen > 0 && p[vlen-1] == '\n') vlen--;

        snprintf(buf, sz, "%.*s", (int)vlen, p);
        found = true;
        break;
    }
    fclose(f);
    return found;
}

// -------------------------------------------------------------------
// cmdTagList  –  list all tags with their commit and date.
// -------------------------------------------------------------------
static int cmdTagList(void) {
    if (!ensureTagsDir()) return -1;

    DIR *dir = opendir(VC_TAGS_DIR);
    if (dir == NULL) {
        printf("No tags found.\n");
        return 0;
    }

    struct dirent *entry;
    bool any = false;
    printf("Tags:\n\n");
    printf("  %-20s  %-26s  %-24s  %s\n", "Name", "Commit", "Date", "Message");
    printf("  %-20s  %-26s  %-24s\n",
           "--------------------",
           "--------------------------",
           "------------------------");

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[MAX_DIR_PATH];
        snprintf(path, sizeof(path), "%s/%s", VC_TAGS_DIR, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        char commit[128] = "(none)";
        char date[64]    = "(unknown)";
        char message[256] = "";

        readTagField(path, "commit",  commit,  sizeof(commit));
        readTagField(path, "date",    date,    sizeof(date));
        readTagField(path, "message", message, sizeof(message));

        if (message[0] != '\0')
            printf("  %-20s  %-26s  %-24s  \"%s\"\n",
                   entry->d_name, commit, date, message);
        else
            printf("  %-20s  %-26s  %s\n",
                   entry->d_name, commit, date);

        any = true;
    }

    closedir(dir);

    if (!any)
        printf("  (no tags yet)\n");
    printf("\n");
    return 0;
}

// -------------------------------------------------------------------
// cmdTagCreate  –  create a new tag pointing to the current commit.
// -------------------------------------------------------------------
static int cmdTagCreate(const char *name, const char *message) {
    if (!isValidTagName(name)) {
        fprintf(stderr, "vcTag: invalid tag name '%s'.\n"
                        "       Names may not start with '-' or contain "
                        "spaces or '/' characters.\n", name);
        return -1;
    }

    if (tagExists(name)) {
        fprintf(stderr, "vcTag: tag '%s' already exists.\n"
                        "       Use 'vc tag --delete %s' to remove it first.\n",
                name, name);
        return -1;
    }

    if (!ensureTagsDir()) return -1;

    // Read the current branch's HEAD_COMMIT to get the zip we're tagging.
    char *branch = vcBranchCurrentName();
    if (branch == NULL) {
        fprintf(stderr, "vcTag: cannot determine current branch.\n");
        return -1;
    }

    char headCommitPath[MAX_DIR_PATH];
    snprintf(headCommitPath, sizeof(headCommitPath),
             ".vc/branches/%s/HEAD_COMMIT", branch);

    char commitZip[MAX_DIR_PATH] = "";
    FILE *hf = fopen(headCommitPath, "r");
    if (hf != NULL) {
        if (fgets(commitZip, sizeof(commitZip), hf) != NULL) {
            size_t len = strlen(commitZip);
            if (len > 0 && commitZip[len-1] == '\n')
                commitZip[--len] = '\0';
        }
        fclose(hf);
    }

    if (commitZip[0] == '\0') {
        fprintf(stderr, "vcTag: no commits found on branch '%s'.\n"
                        "       Make a commit before creating a tag.\n", branch);
        free(branch);
        return -1;
    }

    // Get message interactively if not supplied.
    char *msg = NULL;
    bool mustFree = false;
    if (message != NULL && message[0] != '\0') {
        msg = (char *)message;
    } else {
        msg = readline("Tag message (optional, Enter to skip): ");
        mustFree = true;
    }

    // Build timestamp.
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm);

    // Author from config.
    extern Config *config;
    const char *author = (config && config->user[0]) ? config->user : "unknown";

    // Write the tag file.
    char tagPath[MAX_DIR_PATH];
    snprintf(tagPath, sizeof(tagPath), "%s/%s", VC_TAGS_DIR, name);

    FILE *f = fopen(tagPath, "w");
    if (f == NULL) {
        fprintf(stderr, "vcTag: cannot create tag file '%s': %s\n",
                tagPath, strerror(errno));
        if (mustFree) free(msg);
        free(branch);
        return -1;
    }

    fprintf(f, "tag     = %s\n", name);
    fprintf(f, "commit  = %s\n", commitZip);
    fprintf(f, "branch  = %s\n", branch);
    fprintf(f, "author  = %s\n", author);
    fprintf(f, "date    = %s\n", timeBuf);
    if (msg && msg[0] != '\0')
        fprintf(f, "message = %s\n", msg);
    fclose(f);

    vcLog("tag: %s  commit=%s  branch=%s\n", name, commitZip, branch);

    printf("Tag '%s' created.\n", name);
    printf("  Commit  : %s\n", commitZip);
    printf("  Branch  : %s\n", branch);
    printf("  Date    : %s\n", timeBuf);
    if (msg && msg[0] != '\0')
        printf("  Message : %s\n", msg);

    if (mustFree) free(msg);
    free(branch);
    return 0;
}

// -------------------------------------------------------------------
// cmdTagShow  –  display full details of a tag.
// -------------------------------------------------------------------
static int cmdTagShow(const char *name) {
    if (!tagExists(name)) {
        fprintf(stderr, "vcTag: tag '%s' does not exist.\n", name);
        return -1;
    }

    char tagPath[MAX_DIR_PATH];
    snprintf(tagPath, sizeof(tagPath), "%s/%s", VC_TAGS_DIR, name);

    FILE *f = fopen(tagPath, "r");
    if (f == NULL) {
        fprintf(stderr, "vcTag: cannot open tag '%s': %s\n",
                name, strerror(errno));
        return -1;
    }

    printf("Tag: %s\n\n", name);
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        // Skip the "tag = " line (already printed above).
        if (strncmp(line, "tag ", 4) == 0) continue;
        printf("  %s", line);
    }
    printf("\n");
    fclose(f);
    return 0;
}

// -------------------------------------------------------------------
// cmdTagDelete  –  remove a tag.
// -------------------------------------------------------------------
static int cmdTagDelete(const char *name) {
    if (!tagExists(name)) {
        fprintf(stderr, "vcTag: tag '%s' does not exist.\n", name);
        return -1;
    }

    char tagPath[MAX_DIR_PATH];
    snprintf(tagPath, sizeof(tagPath), "%s/%s", VC_TAGS_DIR, name);

    if (remove(tagPath) != 0) {
        fprintf(stderr, "vcTag: cannot delete tag '%s': %s\n",
                name, strerror(errno));
        return -1;
    }

    printf("Tag '%s' deleted.\n", name);
    return 0;
}

// -------------------------------------------------------------------
// vcTagEnsureInit  –  called by vcInit to create .vc/tags/.
// -------------------------------------------------------------------
int vcTagEnsureInit(void) {
    return ensureTagsDir() ? 0 : -1;
}

// -------------------------------------------------------------------
// vcTag  –  entry point called from vc.c.
//
// Usage:
//   vc tag                          List all tags
//   vc tag <name>                   Create a tag at the current commit
//   vc tag <name> --msg "text"      Create a tag with a message
//   vc tag --show <name>            Show tag details
//   vc tag --delete <name>          Delete a tag
// -------------------------------------------------------------------
int vcTag(int argc, char *argv[]) {

    vcLog("%s %s\n", __func__, vcTopDir);

    // No argument – list tags.
    if (argc < 3 || argv[2] == NULL || argv[2][0] == '\0')
        return cmdTagList();

    // --show <name> or --show=<name>
    if (strcmp(argv[2], "--show") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcTag: --show requires a tag name.\n");
            return -1;
        }
        return cmdTagShow(argv[3]);
    }
    if (strncmp(argv[2], "--show=", 7) == 0)
        return cmdTagShow(argv[2] + 7);

    // --delete <name> or --delete=<name>
    if (strcmp(argv[2], "--delete") == 0) {
        if (argc < 4) {
            fprintf(stderr, "vcTag: --delete requires a tag name.\n");
            return -1;
        }
        return cmdTagDelete(argv[3]);
    }
    if (strncmp(argv[2], "--delete=", 9) == 0)
        return cmdTagDelete(argv[2] + 9);

    // <name> [--msg "text"]  –  create a tag.
    const char *name = argv[2];
    const char *message = NULL;

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--msg=", 6) == 0) {
            message = argv[i] + 6;
            break;
        } else if (strcmp(argv[i], "--msg") == 0 && i + 1 < argc) {
            message = argv[++i];
            break;
        }
    }

    return cmdTagCreate(name, message);
}
