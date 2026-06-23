/*
 * vcd.c
 *
 * Copyright (c) 2026 Kelly Wiles.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// vcd.c  –  vc daemon (vcd) — remote repository server for the vc
//           version control system.
//
// vcd listens on a TCP port (default 9876) and handles push, pull,
// clone, and repository management requests from vc clients.
// It runs as root to bind the port, then drops privileges to the
// configured runUser (default: nobody) before accepting connections.
//
// Build:
//   gcc -o vcd vcd.c vcSha256.c -lcrypt
//
// First-time setup:
//   sudo vcd --init --reporoot /sas/repos
//   sudo vcd --adduser <username>
//   sudo vcd --initrepo users/<username>/<reponame>
//   sudo vcd --start
//
// Admin commands:
//   sudo vcd --adduser    <name>    Add a user account
//   sudo vcd --deleteuser <name>    Remove a user account (keeps repo files)
//   sudo vcd --passwd     <name>    Change a user's password
//   sudo vcd --listusers            List all users and their repo directories
//   sudo vcd --initrepo   <path>    Create a new repository
//   sudo vcd --start                Start the daemon
//   sudo vcd --version              Print version and build date
//   sudo vcd --testhash   <pw>      Diagnostic: show SHA-256 and crypt hash
//   sudo vcd --sethash    <u> <h>   Diagnostic: write SHA-256 hash to users.db
//
// Wire protocol (v2.0 — line-based TCP):
//   C→S  HELLO 2.0
//   S→C  OK vcd/2.0
//   C→S  AUTH <username> <sha256hex>
//   S→C  OK welcome <username>
//   C→S  PUSH <repopath> <relfile> <size>  → binary data
//   C→S  PULL <repopath> <relfile>         → S sends OK <size> then data
//   C→S  LIST <repopath>                   → OK N files\n...\nEND
//   C→S  LISTREPOS                         → OK\n repo [tag]\n...\nEND
//   C→S  INITREPO <repopath>              → create repo if not exists
//   C→S  MOVEREPO <srcpath> <dstpath>     → move repo on server
//   C→S  QUIT
//
// Server repo layout:
//   repoRoot/users/<username>/<reponame>/   personal repos (owner-only)
//   repoRoot/shared/<reponame>/             shared repos (allowedUsers)
//
// Config file (<repoRoot>/vcd.conf):
//   port     = 9876
//   repoRoot = /sas/repos
//   userDb   = /sas/repos/users.db   (derived from repoRoot if omitted)
//   logFile  = /sas/repos/vcd.log    (derived from repoRoot if omitted)
//   runUser  = nobody
//
// User DB (<repoRoot>/users.db) — one entry per line:
//   username:crypt(sha256(password)):userdir
//   kelly:$6$salt$hash:users/kelly
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <crypt.h>
#include <pwd.h>
#include <grp.h>

#include "vcd.h"
#include "vcSha256.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// User DB and config live alongside repoRoot so the daemon can
// read them after dropping privileges to nobody.
// Default paths — override with --conf or vcd.conf settings.
#define VCD_CONF_FILE   "/ssd/repo/vcd.conf"
#define VCD_USER_DB     "/ssd/repo/users.db"
#define VCD_LOG_FILE    "/ssd/repo/vcd.log"
#define VCD_REPO_ROOT   "/ssd/repo"
#define VCD_CONF_DIR    "/ssd/repo"
#define VCD_BACKLOG     16

typedef struct {
    int  port;
    char repoRoot[VCD_MAX_PATH];
    char userDb[VCD_MAX_PATH];
    char logFile[VCD_MAX_PATH];
    char runUser[64];           // drop privileges to this OS user after bind
} VcdConfig;

static VcdConfig cfg = {
    .port     = VCD_DEFAULT_PORT,
    .repoRoot = VCD_REPO_ROOT,
    .userDb   = VCD_USER_DB,
    .logFile  = VCD_LOG_FILE,
    .runUser  = "nobody",       // default — override in vcd.conf
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE *logFp = NULL;

static void vlog(const char *fmt, ...) {
    if (!logFp) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(logFp, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    vfprintf(logFp, fmt, ap);
    va_end(ap);
    fflush(logFp);
}

// ---------------------------------------------------------------------------
// User database
// ---------------------------------------------------------------------------
typedef struct {
    char username[VCD_MAX_USERNAME];
    char cryptedPw[256];
    char userDir[VCD_MAX_PATH];   // relative to repoRoot
} VcdUser;

static bool db_load_user(const char *username, VcdUser *out) {
    FILE *f = fopen(cfg.userDb, "r");
    if (!f) {
        vlog("db_load_user: cannot open '%s': %s\n", cfg.userDb, strerror(errno));
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[--l] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *tok = strtok(line, ":");
        if (!tok || strcmp(tok, username) != 0) continue;

        snprintf(out->username, sizeof(out->username), "%s", tok);

        tok = strtok(NULL, ":");
        if (!tok) { fclose(f); return false; }
        snprintf(out->cryptedPw, sizeof(out->cryptedPw), "%s", tok);

        tok = strtok(NULL, ":");
        if (tok) {
            size_t tl = strlen(tok);
            while (tl > 0 && (tok[tl-1]=='\n'||tok[tl-1]=='\r'||tok[tl-1]==' '))
                tok[--tl] = '\0';
            snprintf(out->userDir, sizeof(out->userDir), "%s", tok);
        } else {
            // Backward compat: default to users/<username>
            snprintf(out->userDir, sizeof(out->userDir),
                     "%s/%s", VCD_USERS_DIR, username);
        }

        fclose(f);
        return true;
    }
    fclose(f);
    return false;
}

static bool db_authenticate(const char *username, const char *pwHash,
                             VcdUser *out) {
    vlog("AUTH: user='%s' db='%s'\n", username, cfg.userDb);
    if (!db_load_user(username, out)) {
        vlog("AUTH FAIL: user '%s' not found in '%s'\n", username, cfg.userDb);
        return false;
    }
    struct crypt_data cd = {0};
    char *result = crypt_r(pwHash, out->cryptedPw, &cd);
    bool ok = result && strcmp(result, out->cryptedPw) == 0;
    if (!ok) vlog("AUTH FAIL: hash mismatch for user '%s'\n", username);
    return ok;
}

static bool db_user_exists(const char *username) {
    VcdUser u;
    return db_load_user(username, &u);
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

// Resolve a client-supplied repoPath to a real filesystem path.
// repoPath is e.g. "users/kelly/ushell" or "shared/teamapp" —
// relative to repoRoot. We reject any path containing ".." .
static bool resolve_repo_path(const char *repoPath, char *out, size_t sz) {
    // Reject path traversal.
    if (strstr(repoPath, "..")) return false;
    // Must start with "users/" or "shared/".
    if (strncmp(repoPath, VCD_USERS_DIR "/", strlen(VCD_USERS_DIR) + 1) != 0 &&
        strncmp(repoPath, VCD_SHARED_DIR "/", strlen(VCD_SHARED_DIR) + 1) != 0)
        return false;
    snprintf(out, sz, "%s/%s", cfg.repoRoot, repoPath);
    return true;
}

static bool repo_exists(const char *repoPath) {
    char full[VCD_MAX_PATH];
    if (!resolve_repo_path(repoPath, full, sizeof(full))) return false;
    char vcdir[VCD_MAX_PATH];
    snprintf(vcdir, sizeof(vcdir), "%s/.vc", full);
    struct stat st;
    return stat(vcdir, &st) == 0 && S_ISDIR(st.st_mode);
}

// Return true if the repo's config.vc has private = true.
static bool repo_is_private(const char *repoPath) {
    char cfgPath[VCD_MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s/%s/.vc/config.vc",
             cfg.repoRoot, repoPath);
    FILE *f = fopen(cfgPath, "r");
    if (!f) return false;  // no config — treat as public
    char line[512];
    bool priv = false;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        while (*key == ' ') key++;
        while (*val == ' ') val++;
        size_t kl = strlen(key);
        while (kl > 0 && (key[kl-1]==' '||key[kl-1]=='\n')) key[--kl]='\0';
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl-1]==' '||val[vl-1]=='\n')) val[--vl]='\0';
        if (strcasecmp(key, "private") == 0) {
            priv = (strcasecmp(val, "true") == 0 ||
                    strcasecmp(val, "yes")  == 0 ||
                    strcmp(val, "1") == 0);
            break;
        }
    }
    fclose(f);
    return priv;
}

// Check the authenticated user can access the repo.
// Policy: only the repo owner (matching userDir prefix) may push or pull.
static bool user_can_access(const VcdUser *user, const char *repoPath) {
    // Personal repo: must be under the user's own directory.
    if (strncmp(repoPath, VCD_USERS_DIR "/", strlen(VCD_USERS_DIR) + 1) == 0) {
        // repoPath = "users/kelly/ushell" — check it starts with userDir.
        return strncmp(repoPath, user->userDir, strlen(user->userDir)) == 0;
    }
    // Shared repo: user must be listed in the repo's allowedUsers.
    // (Stored in repoRoot/shared/<name>/.vc/config.vc as allowedUsers = ...)
    if (strncmp(repoPath, VCD_SHARED_DIR "/", strlen(VCD_SHARED_DIR) + 1) == 0) {
        char cfgPath[VCD_MAX_PATH];
        snprintf(cfgPath, sizeof(cfgPath), "%s/%s/.vc/config.vc",
                 cfg.repoRoot, repoPath);
        FILE *f = fopen(cfgPath, "r");
        if (!f) return false;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line, *val = eq + 1;
            while (*key == ' ') key++;
            while (*val == ' ') val++;
            size_t kl = strlen(key);
            while (kl > 0 && (key[kl-1]==' '||key[kl-1]=='\n')) key[--kl]='\0';
            size_t vl = strlen(val);
            while (vl > 0 && (val[vl-1]==' '||val[vl-1]=='\n')) val[--vl]='\0';
            if (strcasecmp(key, "allowedUsers") == 0) {
                fclose(f);
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", val);
                char *tok = strtok(buf, ",");
                while (tok) {
                    while (*tok == ' ') tok++;
                    size_t tl = strlen(tok);
                    while (tl > 0 && tok[tl-1]==' ') tok[--tl]='\0';
                    if (strcmp(tok, user->username) == 0) return true;
                    tok = strtok(NULL, ",");
                }
                return false;
            }
        }
        fclose(f);
        return false;
    }
    return false;
}

static void mkdirp(const char *path) {
    char tmp[VCD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static bool send_line(int fd, const char *fmt, ...) {
    char buf[VCD_MAX_LINE];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[n] = '\n'; buf[n+1] = '\0';
    return write(fd, buf, (size_t)(n + 1)) == n + 1;
}

static bool recv_line(int fd, char *buf, size_t sz) {
    size_t n = 0;
    while (n < sz - 1) {
        char c; ssize_t r = read(fd, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return n > 0;
}

static bool send_data(int fd, const void *data, size_t sz) {
    const char *p = (const char *)data; size_t sent = 0;
    while (sent < sz) {
        ssize_t n = write(fd, p + sent, sz - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recv_data(int fd, void *buf, size_t sz) {
    char *p = (char *)buf; size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, p + got, sz - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

// PUSH <repoPath> <relFile> <size>
static void handle_push(int fd, const char *repoPath, const char *relFile,
                         long long size, const VcdUser *user) {
    if (!repo_exists(repoPath)) {
        send_line(fd, "ERROR repo '%s' not found", repoPath);
        return;
    }
    if (!user_can_access(user, repoPath)) {
        send_line(fd, "ERROR access denied to '%s'", repoPath);
        vlog("PUSH denied: user=%s repo=%s\n", user->username, repoPath);
        return;
    }
    if (strstr(relFile, "..")) {
        send_line(fd, "ERROR invalid file path");
        return;
    }

    char destPath[VCD_MAX_PATH];
    snprintf(destPath, sizeof(destPath), "%s/%s/%s",
             cfg.repoRoot, repoPath, relFile);

    // Ensure parent directory exists.
    char parent[VCD_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", destPath);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; mkdirp(parent); }

    // Verify parent is writable before accepting data.
    if (slash && access(parent, W_OK) != 0) {
        send_line(fd, "ERROR cannot write to repo: %s", strerror(errno));
        return;
    }

    send_line(fd, "OK ready");

    // Receive file data into temp file.
    char tmp[VCD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", destPath);
    FILE *f = fopen(tmp, "wb");
    if (!f) { send_line(fd, "ERROR cannot create file"); return; }

    char buf[VCD_TRANSFER_BUF];
    long long remaining = size;
    bool ok = true;
    while (remaining > 0) {
        size_t chunk = (size_t)(remaining > (long long)sizeof(buf)
                       ? sizeof(buf) : remaining);
        if (!recv_data(fd, buf, chunk)) { ok = false; break; }
        if (fwrite(buf, 1, chunk, f) != chunk) { ok = false; break; }
        remaining -= (long long)chunk;
    }
    fclose(f);

    if (ok && rename(tmp, destPath) == 0) {
        send_line(fd, "OK %lld bytes", size);
        vlog("PUSH ok: user=%s repo=%s file=%s size=%lld\n",
             user->username, repoPath, relFile, size);
    } else {
        remove(tmp);
        send_line(fd, "ERROR transfer failed");
    }
}

// PULL <repoPath> <relFile>
static void handle_pull(int fd, const char *repoPath, const char *relFile,
                         const VcdUser *user) {
    if (!repo_exists(repoPath)) {
        send_line(fd, "ERROR repo '%s' not found", repoPath);
        return;
    }
    if (!user_can_access(user, repoPath)) {
        send_line(fd, "ERROR access denied to '%s'", repoPath);
        vlog("PULL denied: user=%s repo=%s\n", user->username, repoPath);
        return;
    }
    if (strstr(relFile, "..")) {
        send_line(fd, "ERROR invalid file path");
        return;
    }

    char srcPath[VCD_MAX_PATH];
    snprintf(srcPath, sizeof(srcPath), "%s/%s/%s",
             cfg.repoRoot, repoPath, relFile);

    struct stat st;
    if (stat(srcPath, &st) != 0) {
        send_line(fd, "ERROR file not found: %s", relFile);
        return;
    }

    // Open file before committing to send OK.
    FILE *f = fopen(srcPath, "rb");
    if (!f) {
        send_line(fd, "ERROR cannot open file: %s", strerror(errno));
        return;
    }

    send_line(fd, "OK %lld", (long long)st.st_size);

    char buf[VCD_TRANSFER_BUF];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (!send_data(fd, buf, n)) { ok = false; break; }
    }
    fclose(f);
    if (ok) vlog("PULL ok: user=%s repo=%s file=%s\n",
                 user->username, repoPath, relFile);
}

// LIST <repoPath>
static void handle_list(int fd, const char *repoPath,
                          const VcdUser *user) {
    if (!repo_exists(repoPath)) {
        send_line(fd, "ERROR repo '%s' not found", repoPath);
        return;
    }
    if (!user_can_access(user, repoPath)) {
        send_line(fd, "ERROR access denied to '%s'", repoPath);
        return;
    }

    char vcDir[VCD_MAX_PATH];
    snprintf(vcDir, sizeof(vcDir), "%s/%s/.vc", cfg.repoRoot, repoPath);

    // Count files first.
    char cmd[VCD_MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "find '%s' -type f 2>/dev/null", vcDir);
    FILE *fp = popen(cmd, "r");
    if (!fp) { send_line(fd, "ERROR cannot list repo"); return; }
    int count = 0;
    char line[VCD_MAX_PATH];
    while (fgets(line, sizeof(line), fp)) count++;
    pclose(fp);

    send_line(fd, "OK %d files", count);

    fp = popen(cmd, "r");
    if (!fp) { send_line(fd, "END"); return; }

    // Strip repoRoot/repoPath/ prefix from each path.
    char base[VCD_MAX_PATH];
    snprintf(base, sizeof(base), "%s/%s/", cfg.repoRoot, repoPath);
    size_t baseLen = strlen(base);

    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[--l] = '\0';
        struct stat st;
        if (stat(line, &st) != 0) continue;
        const char *rel = (strncmp(line, base, baseLen) == 0)
                          ? line + baseLen : line;
        send_line(fd, "%s %lld", rel, (long long)st.st_size);
    }
    pclose(fp);
    send_line(fd, "END");
}

// LISTREPOS
// Returns repos the authenticated user can see:
//   1. All of the user's own personal repos (users/<username>/*)
//   2. All shared repos they can access (shared/*)
//   3. Other users' public repos (users/<other>/*/private=false)
static void handle_listrepos(int fd, const VcdUser *user) {
    send_line(fd, "OK");

    // 1. User's own personal repos.
    char userDir[VCD_MAX_PATH];
    snprintf(userDir, sizeof(userDir), "%s/%s", cfg.repoRoot, user->userDir);
    DIR *d = opendir(userDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char repoPath[VCD_MAX_PATH];
            snprintf(repoPath, sizeof(repoPath), "%s/%s",
                     user->userDir, de->d_name);
            if (repo_exists(repoPath))
                send_line(fd, "%s [mine]", repoPath);
        }
        closedir(d);
    }

    // 2. Shared repos the user can access.
    char sharedDir[VCD_MAX_PATH];
    snprintf(sharedDir, sizeof(sharedDir), "%s/%s",
             cfg.repoRoot, VCD_SHARED_DIR);
    d = opendir(sharedDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char repoPath[VCD_MAX_PATH];
            snprintf(repoPath, sizeof(repoPath), "%s/%s",
                     VCD_SHARED_DIR, de->d_name);
            if (repo_exists(repoPath) && user_can_access(user, repoPath))
                send_line(fd, "%s [shared]", repoPath);
        }
        closedir(d);
    }

    // 3. Other users' public repos (private = false).
    char usersRoot[VCD_MAX_PATH];
    snprintf(usersRoot, sizeof(usersRoot), "%s/%s",
             cfg.repoRoot, VCD_USERS_DIR);
    DIR *ud = opendir(usersRoot);
    if (ud) {
        struct dirent *ude;
        while ((ude = readdir(ud))) {
            if (ude->d_name[0] == '.') continue;
            // Skip the authenticated user's own directory (already listed).
            char thisUserDir[VCD_MAX_PATH];
            snprintf(thisUserDir, sizeof(thisUserDir), "%s/%s",
                     VCD_USERS_DIR, ude->d_name);
            if (strcmp(thisUserDir, user->userDir) == 0) continue;

            // Walk each other user's repos.
            char otherDir[VCD_MAX_PATH];
            snprintf(otherDir, sizeof(otherDir), "%s/%s",
                     usersRoot, ude->d_name);
            DIR *rd = opendir(otherDir);
            if (!rd) continue;
            struct dirent *rde;
            while ((rde = readdir(rd))) {
                if (rde->d_name[0] == '.') continue;
                char repoPath[VCD_MAX_PATH];
                snprintf(repoPath, sizeof(repoPath), "%s/%s",
                         thisUserDir, rde->d_name);
                if (!repo_exists(repoPath)) continue;
                // Only include if private = false.
                if (!repo_is_private(repoPath))
                    send_line(fd, "%s [public]", repoPath);
            }
            closedir(rd);
        }
        closedir(ud);
    }

    send_line(fd, "END");
}

// INITREPO <repoPath>
// Create and initialise a repo directory if it doesn't exist.
// Only allowed under the authenticated user's own directory.
static void handle_initrepo(int fd, const char *repoPath,
                              const VcdUser *user) {
    if (strstr(repoPath, "..")) {
        send_line(fd, "ERROR invalid path");
        return;
    }
    // Personal repos: must be under the user's own userDir.
    if (strncmp(repoPath, VCD_USERS_DIR "/",
                strlen(VCD_USERS_DIR) + 1) == 0) {
        if (strncmp(repoPath, user->userDir, strlen(user->userDir)) != 0) {
            send_line(fd, "ERROR you may only create repos under %s",
                      user->userDir);
            return;
        }
    }
    // Shared repos: must be explicitly allowed — not self-service.
    else if (strncmp(repoPath, VCD_SHARED_DIR "/",
                     strlen(VCD_SHARED_DIR) + 1) == 0) {
        send_line(fd, "ERROR shared repos must be created by the admin: "
                      "sudo vcd --initrepo %s", repoPath);
        return;
    } else {
        send_line(fd, "ERROR repo path must start with '%s/' or '%s/'",
                  VCD_USERS_DIR, VCD_SHARED_DIR);
        return;
    }

    if (repo_exists(repoPath)) {
        send_line(fd, "OK exists");
        vlog("INITREPO: already exists: %s\n", repoPath);
        return;
    }

    char fullPath[VCD_MAX_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", cfg.repoRoot, repoPath);

    const char *subdirs[] = {
        "", "/.vc", "/.vc/data", "/.vc/branches",
        "/.vc/branches/main", "/.vc/tags", "/.vc/stash", NULL
    };
    for (int i = 0; subdirs[i]; i++) {
        char path[VCD_MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", fullPath, subdirs[i]);
        // Use mkdirp for the base dir (creates parents like users/kelly/).
        // Regular mkdir is fine for subdirs since parent already exists.
        if (i == 0)
            mkdirp(path);
        else if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            send_line(fd, "ERROR cannot create directory: %s",
                      strerror(errno));
            vlog("INITREPO error: mkdir %s: %s\n", path, strerror(errno));
            return;
        }
    }

    // Write HEAD.
    char p[VCD_MAX_PATH];
    snprintf(p, sizeof(p), "%s/.vc/HEAD", fullPath);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "main\n"); fclose(f); }

    // Write empty branch index.
    snprintf(p, sizeof(p), "%s/.vc/branches/main/index", fullPath);
    f = fopen(p, "w");
    if (f) fclose(f);

    // Minimal config.vc.
    const char *proj = strrchr(repoPath, '/');
    proj = proj ? proj + 1 : repoPath;
    snprintf(p, sizeof(p), "%s/.vc/config.vc", fullPath);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "[Repo]\n");
        fprintf(f, "  project = %s\n", proj);
        fprintf(f, "  repo    = %s\n", repoPath);
        fprintf(f, "  owner   = %s\n", user->username);
        fprintf(f, "  private = false\n");
        fclose(f);
    }

    send_line(fd, "OK created");
    vlog("INITREPO: user=%s created %s\n", user->username, repoPath);
}

// ---------------------------------------------------------------------------
// Client session
// ---------------------------------------------------------------------------
// MOVEREPO <srcPath> <dstPath>
// Move a repo from one location to another (e.g. users/kelly/app → shared/app).
// Rules:
//   - Source must exist and be owned by the authenticated user.
//   - Destination must not already exist.
//   - Both paths must be under users/ or shared/.
//   - User can move FROM their own users/ dir to anywhere valid.
//   - User can move FROM shared/ only if they are the repo owner.
static void handle_moverepo(int fd, const char *srcPath, const char *dstPath,
                              const VcdUser *user) {
    // Validate paths.
    if (strstr(srcPath, "..") || strstr(dstPath, "..")) {
        send_line(fd, "ERROR invalid path — '..' not allowed");
        return;
    }

    bool srcIsUsers  = strncmp(srcPath, VCD_USERS_DIR "/",  strlen(VCD_USERS_DIR)  + 1) == 0;
    bool srcIsShared = strncmp(srcPath, VCD_SHARED_DIR "/", strlen(VCD_SHARED_DIR) + 1) == 0;
    bool dstIsUsers  = strncmp(dstPath, VCD_USERS_DIR "/",  strlen(VCD_USERS_DIR)  + 1) == 0;
    bool dstIsShared = strncmp(dstPath, VCD_SHARED_DIR "/", strlen(VCD_SHARED_DIR) + 1) == 0;

    if ((!srcIsUsers && !srcIsShared) || (!dstIsUsers && !dstIsShared)) {
        send_line(fd, "ERROR paths must start with '%s/' or '%s/'",
                  VCD_USERS_DIR, VCD_SHARED_DIR);
        return;
    }

    if (!repo_exists(srcPath)) {
        send_line(fd, "ERROR source repo '%s' not found", srcPath);
        return;
    }

    // Permission: user must own the source repo.
    if (!user_can_access(user, srcPath)) {
        send_line(fd, "ERROR access denied — you do not own '%s'", srcPath);
        vlog("MOVEREPO denied: user=%s src=%s\n", user->username, srcPath);
        return;
    }

    // If moving to users/, it must be under the requesting user's own dir.
    if (dstIsUsers &&
        strncmp(dstPath, user->userDir, strlen(user->userDir)) != 0) {
        send_line(fd, "ERROR you may only move repos into your own directory (%s/%s)",
                  cfg.repoRoot, user->userDir);
        return;
    }

    char srcFull[VCD_MAX_PATH], dstFull[VCD_MAX_PATH];
    snprintf(srcFull, sizeof(srcFull), "%s/%s", cfg.repoRoot, srcPath);
    snprintf(dstFull, sizeof(dstFull), "%s/%s", cfg.repoRoot, dstPath);

    // Destination must not already exist.
    struct stat st;
    if (stat(dstFull, &st) == 0) {
        send_line(fd, "ERROR destination '%s' already exists", dstPath);
        return;
    }

    // Ensure destination parent directory exists.
    char dstParent[VCD_MAX_PATH];
    snprintf(dstParent, sizeof(dstParent), "%s", dstFull);
    char *slash = strrchr(dstParent, '/');
    if (slash) { *slash = '\0'; mkdirp(dstParent); }

    // Update repo's own config.vc with new path before moving.
    char cfgPath[VCD_MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s/.vc/config.vc", srcFull);
    // Read, update repo field, write back.
    FILE *f = fopen(cfgPath, "r");
    if (f) {
        char lines[128][VCD_MAX_PATH];
        int  nlines = 0;
        char line[VCD_MAX_PATH];
        while (nlines < 128 && fgets(line, sizeof(line), f))
            snprintf(lines[nlines++], VCD_MAX_PATH, "%s", line);
        fclose(f);
        f = fopen(cfgPath, "w");
        if (f) {
            for (int i = 0; i < nlines; i++) {
                if (strncmp(lines[i], "  repo", 6) == 0)
                    fprintf(f, "  repo    = %s/%s\n", cfg.repoRoot, dstPath);
                else
                    fputs(lines[i], f);
            }
            fclose(f);
        }
    }

    // Perform the move.
    if (rename(srcFull, dstFull) != 0) {
        send_line(fd, "ERROR move failed: %s", strerror(errno));
        vlog("MOVEREPO error: %s -> %s: %s\n", srcFull, dstFull, strerror(errno));
        return;
    }

    send_line(fd, "OK %s", dstPath);
    vlog("MOVEREPO: user=%s %s -> %s\n", user->username, srcPath, dstPath);
}

static void handle_client(int fd, const char *addr) {
    vlog("Connect: %s\n", addr);

    char line[VCD_MAX_LINE];

    // 1. Greeting.
    send_line(fd, "OK vcd/%s", VCD_VERSION);

    // 2. HELLO.
    if (!recv_line(fd, line, sizeof(line)) ||
        strncmp(line, CMD_HELLO, 5) != 0) {
        send_line(fd, "ERROR expected HELLO");
        goto done;
    }
    send_line(fd, "OK");
    vlog("HELLO from %s\n", addr);

    // 3. AUTH.
    if (!recv_line(fd, line, sizeof(line)) ||
        strncmp(line, CMD_AUTH, 4) != 0) {
        send_line(fd, "ERROR expected AUTH");
        goto done;
    }
    char authUser[VCD_MAX_USERNAME] = "";
    char authHash[256] = "";
    sscanf(line + 5, "%63s %255s", authUser, authHash);

    VcdUser user;
    if (!db_authenticate(authUser, authHash, &user)) {
        send_line(fd, "ERROR unauthorized");
        vlog("AUTH failed: user=%s addr=%s\n", authUser, addr);
        goto done;
    }
    send_line(fd, "OK welcome %s", authUser);
    vlog("AUTH ok: user=%s addr=%s\n", authUser, addr);

    // 4. Command loop.
    while (recv_line(fd, line, sizeof(line))) {
        vlog("CMD [%s]: %s\n", authUser, line);

        if (strncmp(line, CMD_QUIT, 4) == 0) {
            send_line(fd, "OK bye");
            break;
        }

        if (strncmp(line, CMD_PUSH, 4) == 0) {
            char repo[VCD_MAX_PATH], relFile[VCD_MAX_PATH];
            long long sz = 0;
            if (sscanf(line + 5, "%511s %511s %lld", repo, relFile, &sz) != 3) {
                send_line(fd, "ERROR usage: PUSH <repo> <file> <size>");
                continue;
            }
            handle_push(fd, repo, relFile, sz, &user);
            continue;
        }

        if (strncmp(line, CMD_PULL, 4) == 0) {
            char repo[VCD_MAX_PATH], relFile[VCD_MAX_PATH];
            if (sscanf(line + 5, "%511s %511s", repo, relFile) != 2) {
                send_line(fd, "ERROR usage: PULL <repo> <file>");
                continue;
            }
            handle_pull(fd, repo, relFile, &user);
            continue;
        }

        // LISTREPOS must be checked before LIST since LIST is a prefix of it.
        if (strcmp(line, CMD_LISTREPOS) == 0) {
            handle_listrepos(fd, &user);
            continue;
        }

        if (strncmp(line, CMD_LIST, 4) == 0) {
            char repo[VCD_MAX_PATH];
            if (sscanf(line + 5, "%511s", repo) != 1) {
                send_line(fd, "ERROR usage: LIST <repo>");
                continue;
            }
            handle_list(fd, repo, &user);
            continue;
        }

        if (strncmp(line, CMD_MOVEREPO, 8) == 0) {
            char src[VCD_MAX_PATH], dst[VCD_MAX_PATH];
            if (sscanf(line + 9, "%511s %511s", src, dst) != 2) {
                send_line(fd, "ERROR usage: MOVEREPO <srcpath> <dstpath>");
                continue;
            }
            handle_moverepo(fd, src, dst, &user);
            continue;
        }

        if (strncmp(line, CMD_INITREPO, 8) == 0) {
            char repo[VCD_MAX_PATH];
            if (sscanf(line + 9, "%511s", repo) != 1) {
                send_line(fd, "ERROR usage: INITREPO <repopath>");
                continue;
            }
            handle_initrepo(fd, repo, &user);
            continue;
        }

        send_line(fd, "ERROR unknown command");
    }

done:
    vlog("Disconnect: %s\n", addr);
    close(fd);
}

// ---------------------------------------------------------------------------
// Admin commands
// ---------------------------------------------------------------------------
static void read_password(const char *prompt, char *buf, size_t sz) {
    struct termios old, noecho;
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)sz, stdin)) {
        size_t l = strlen(buf);
        if (l > 0 && buf[l-1] == '\n') buf[--l] = '\0';
        if (l > 0 && buf[l-1] == '\r') buf[--l] = '\0';
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    printf("\n");
}

static void sha256hex_server(const char *input, char *out65) {
    vc_sha256_hex(input, out65);
}

// Hash a password for storage.
// The client sends SHA-256(plaintext) over the wire, so we must store
// crypt(SHA-256(plaintext)) to match what arrives during authentication.
static void hash_password(const char *pw, char *out, size_t sz) {
    // Step 1: SHA-256 the plaintext — mirrors what the client sends.
    char pwHash[65];
    sha256hex_server(pw, pwHash);
    if (pwHash[0] == '\0') {
        fprintf(stderr, "vcd: sha256sum not available — cannot hash password.\n");
        out[0] = '\0';
        return;
    }

    // Step 2: crypt the hash with a random SHA-512 salt for secure storage.
    char salt[32];
    snprintf(salt, sizeof(salt), "$6$");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    for (int i = 3; i < 18; i++)
        salt[i] = chars[rand() % (sizeof(chars)-1)];
    salt[18] = '$'; salt[19] = '\0';
    struct crypt_data cd = {0};
    char *r = crypt_r(pwHash, salt, &cd);
    if (r) snprintf(out, sz, "%s", r);
    else out[0] = '\0';

    memset(pwHash, 0, sizeof(pwHash));
}

static int cmd_init(void) {
    // Create config dir.
    if (mkdir(VCD_CONF_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "vcd: cannot create %s: %s\n",
                VCD_CONF_DIR, strerror(errno)); return 1;
    }
    // Create repo root structure.
    char path[VCD_MAX_PATH];
    snprintf(path, sizeof(path), "%s", cfg.repoRoot);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "vcd: cannot create repoRoot %s: %s\n",
                path, strerror(errno)); return 1;
    }
    snprintf(path, sizeof(path), "%s/%s", cfg.repoRoot, VCD_USERS_DIR);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/%s", cfg.repoRoot, VCD_SHARED_DIR);
    mkdir(path, 0755);

    // Write user DB.
    FILE *f = fopen(cfg.userDb, "a");
    if (!f) {
        fprintf(stderr, "vcd: cannot create userDb %s: %s\n",
                cfg.userDb, strerror(errno)); return 1;
    }
    fprintf(f, "# vcd user database\n");
    fprintf(f, "# format: username:crypt_hash:userdir\n");
    fclose(f);
    chmod(cfg.userDb, 0600);

    // Write config file.
    char confPath[VCD_MAX_PATH];
    snprintf(confPath, sizeof(confPath), "%s/vcd.conf", cfg.repoRoot);
    f = fopen(confPath, "w");
    if (f) {
        fprintf(f, "port     = %d\n", cfg.port);
        fprintf(f, "repoRoot = %s\n", cfg.repoRoot);
        fprintf(f, "userDb   = %s\n", cfg.userDb);
        fprintf(f, "logFile  = %s\n", cfg.logFile);
        fprintf(f, "runUser  = %s\n", cfg.runUser);
        fclose(f);
        chmod(confPath, 0644);
    }

    printf("vcd initialised.\n");
    printf("  Config   : %s  (644)\n", confPath);
    printf("  User DB  : %s  (600)\n", cfg.userDb);
    printf("  Repo root: %s\n", cfg.repoRoot);
    printf("    users/ : %s/users/   (personal repos)\n", cfg.repoRoot);
    printf("    shared/: %s/shared/  (shared repos)\n", cfg.repoRoot);
    printf("\nNext: sudo vcd --adduser <name>\n");
    printf("      sudo vcd --start\n");
    return 0;
}

static int cmd_adduser(const char *username) {
    // Validate.
    printf("Config:\n");
    printf("  repoRoot : %s\n", cfg.repoRoot);
    printf("  userDb   : %s\n", cfg.userDb);
    printf("\n");
    if (!username || !username[0]) {
        fprintf(stderr, "vcd: username required\n"); return 1;
    }
    for (const char *p = username; *p; p++) {
        if (isspace((unsigned char)*p)) {
            fprintf(stderr, "vcd: username may not contain whitespace\n");
            return 1;
        }
    }
    if (db_user_exists(username)) {
        fprintf(stderr, "vcd: user '%s' already exists\n", username);
        return 1;
    }

    // Prompt for user directory (default: users/<username>).
    char dirInput[VCD_MAX_PATH] = "";
    char defaultDir[VCD_MAX_PATH];
    snprintf(defaultDir, sizeof(defaultDir), "%s/%s",
             VCD_USERS_DIR, username);
    printf("Repo directory under %s/\n", cfg.repoRoot);
    printf("  Press Enter for '%s': ", defaultDir);
    fflush(stdout);
    if (fgets(dirInput, sizeof(dirInput), stdin)) {
        size_t l = strlen(dirInput);
        if (l > 0 && dirInput[l-1] == '\n') dirInput[--l] = '\0';
    }
    const char *dirName = dirInput[0] ? dirInput : defaultDir;
    if (strstr(dirName, "..")) {
        fprintf(stderr, "vcd: directory may not contain '..'\n"); return 1;
    }

    // Create the user directory.
    char fullDir[VCD_MAX_PATH];
    snprintf(fullDir, sizeof(fullDir), "%s/%s", cfg.repoRoot, dirName);
    char cmd[VCD_MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", fullDir);
    if (system(cmd) != 0) {
        fprintf(stderr, "vcd: cannot create %s\n", fullDir); return 1;
    }

    // Set ownership so the daemon (running as runUser) can access it.
    if (cfg.runUser[0] != '\0' && strcmp(cfg.runUser, "root") != 0) {
        struct passwd *rpw = getpwnam(cfg.runUser);
        if (rpw) {
            if (chown(fullDir, rpw->pw_uid, rpw->pw_gid) != 0)
                vlog("WARNING: cannot chown %s: %s\n", fullDir, strerror(errno));
        }
    }

    printf("Directory: %s\n", fullDir);

    // Password.
    char pw1[256], pw2[256];
    read_password("New password  : ", pw1, sizeof(pw1));
    read_password("Confirm       : ", pw2, sizeof(pw2));
    if (strcmp(pw1, pw2) != 0) {
        fprintf(stderr, "vcd: passwords do not match\n");
        memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2));
        return 1;
    }
    if (strlen(pw1) < 6) {
        fprintf(stderr, "vcd: password too short (min 6 chars)\n");
        memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2));
        return 1;
    }
    char hashed[256];
    hash_password(pw1, hashed, sizeof(hashed));
    memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2));
    if (!hashed[0]) { fprintf(stderr, "vcd: hash failed\n"); return 1; }

    FILE *f = fopen(cfg.userDb, "a");
    if (!f) {
        fprintf(stderr, "vcd: cannot write userDb: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "%s:%s:%s\n", username, hashed, dirName);
    fclose(f);

    printf("\nUser '%s' added.\n", username);
    printf("Repo dir : %s\n", fullDir);
    printf("\nCreate a repo for this user:\n");
    printf("  sudo vcd --initrepo %s/<reponame>\n", dirName);
    printf("\nOn client:\n");
    printf("  vc config --set host    <server-ip>\n");
    printf("  vc config --set vcdUser %s\n", username);
    printf("  vc config --set repo    %s/<reponame>\n", dirName);
    return 0;
}

static int cmd_passwd(const char *username) {
    if (!username || !username[0]) {
        fprintf(stderr, "vcd: --passwd requires a username\n"); return 1;
    }
    printf("Updating password in: %s\n", cfg.userDb);
    VcdUser user;
    if (!db_load_user(username, &user)) {
        fprintf(stderr, "vcd: user '%s' not found\n", username); return 1;
    }
    char pw1[256], pw2[256];
    read_password("New password  : ", pw1, sizeof(pw1));
    read_password("Confirm       : ", pw2, sizeof(pw2));
    if (strcmp(pw1, pw2) != 0) {
        fprintf(stderr, "vcd: passwords do not match\n");
        memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2)); return 1;
    }
    if (strlen(pw1) < 6) {
        fprintf(stderr, "vcd: password too short (min 6 chars)\n");
        memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2)); return 1;
    }
    char hashed[256];
    hash_password(pw1, hashed, sizeof(hashed));
    memset(pw1, 0, sizeof(pw1)); memset(pw2, 0, sizeof(pw2));

    // Rewrite DB preserving other users.
    char tmp[VCD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.userDb);
    FILE *in = fopen(cfg.userDb, "r"), *out = fopen(tmp, "w");
    if (!in || !out) {
        fprintf(stderr, "vcd: cannot update userDb\n");
        if (in) fclose(in); if (out) fclose(out); return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", line);
        char *tok = strtok(copy, ":");
        if (tok && strcmp(tok, username) == 0)
            fprintf(out, "%s:%s:%s\n", username, hashed, user.userDir);
        else
            fputs(line, out);
    }
    fclose(in); fclose(out);
    if (rename(tmp, cfg.userDb) != 0) {
        fprintf(stderr, "vcd: rename %s -> %s failed: %s\n",
                tmp, cfg.userDb, strerror(errno));
        return 1;
    }
    printf("Password updated for '%s' in %s\n", username, cfg.userDb);
    // Verify the file is readable
    FILE *verify = fopen(cfg.userDb, "r");
    if (verify) {
        char line[512];
        while (fgets(line, sizeof(line), verify)) printf("  db: %s", line);
        fclose(verify);
    } else {
        fprintf(stderr, "WARNING: cannot read back %s: %s\n",
                cfg.userDb, strerror(errno));
    }
    return 0;
}

static int cmd_initrepo(const char *repoPath) {
    // repoPath is relative to repoRoot, e.g. "users/kelly/ushell"
    // or "shared/teamapp".
    if (!repoPath || !repoPath[0]) {
        fprintf(stderr, "vcd: --initrepo requires a repo path.\n"
                        "     Example: vcd --initrepo users/kelly/ushell\n"
                        "              vcd --initrepo shared/teamapp\n");
        return 1;
    }
    if (strstr(repoPath, "..")) {
        fprintf(stderr, "vcd: repo path must not contain '..'\n");
        return 1;
    }
    if (strncmp(repoPath, VCD_USERS_DIR "/",  strlen(VCD_USERS_DIR)  + 1) != 0 &&
        strncmp(repoPath, VCD_SHARED_DIR "/", strlen(VCD_SHARED_DIR) + 1) != 0) {
        fprintf(stderr, "vcd: repo path must start with '%s/' or '%s/'\n",
                VCD_USERS_DIR, VCD_SHARED_DIR);
        return 1;
    }

    char fullPath[VCD_MAX_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", cfg.repoRoot, repoPath);

    if (repo_exists(repoPath)) {
        printf("Repo already exists: %s\n", fullPath);
        return 0;
    }

    // Create directory structure.
    const char *subdirs[] = {
        "", "/.vc", "/.vc/data", "/.vc/branches",
        "/.vc/branches/main", "/.vc/tags", "/.vc/stash", NULL
    };
    for (int i = 0; subdirs[i]; i++) {
        char path[VCD_MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", fullPath, subdirs[i]);
        if (i == 0)
            mkdirp(path);
        else if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "vcd: cannot create '%s': %s\n",
                    path, strerror(errno));
            return 1;
        }
    }

    // Write HEAD.
    char headPath[VCD_MAX_PATH];
    snprintf(headPath, sizeof(headPath), "%s/.vc/HEAD", fullPath);
    FILE *f = fopen(headPath, "w");
    if (f) { fprintf(f, "main\n"); fclose(f); }

    // Write empty branch index.
    char idxPath[VCD_MAX_PATH];
    snprintf(idxPath, sizeof(idxPath), "%s/.vc/branches/main/index", fullPath);
    f = fopen(idxPath, "w");
    if (f) fclose(f);

    // Derive project name from last path component.
    const char *lastSlash = strrchr(repoPath, '/');
    const char *projName  = lastSlash ? lastSlash + 1 : repoPath;

    // Write minimal config.vc.
    char cfgPath[VCD_MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s/.vc/config.vc", fullPath);
    f = fopen(cfgPath, "w");
    if (f) {
        fprintf(f, "[Repo]\n");
        fprintf(f, "  project = %s\n", projName);
        fprintf(f, "  repo    = %s\n", fullPath);
        fprintf(f, "  private = false\n");
        fclose(f);
    }

    printf("Repo initialised: %s\n", fullPath);

    // Set ownership so the daemon (running as runUser) can read/write it.
    if (cfg.runUser[0] != '\0' && strcmp(cfg.runUser, "root") != 0) {
        struct passwd *rpw = getpwnam(cfg.runUser);
        if (rpw) {
            if (chown(fullPath, rpw->pw_uid, rpw->pw_gid) != 0)
                vlog("WARNING: cannot chown %s: %s\n", fullPath, strerror(errno));
        }
    }

    printf("\nClient config:\n");
    printf("  vc config --set repo %s\n", repoPath);
    return 0;
}

static int cmd_deleteuser(const char *username) {
    if (!username || !username[0]) {
        fprintf(stderr, "vcd: --deleteuser requires a username\n");
        return 1;
    }

    // Verify user exists.
    VcdUser user;
    if (!db_load_user(username, &user)) {
        fprintf(stderr, "vcd: user '%s' not found in %s\n",
                username, cfg.userDb);
        return 1;
    }

    // Confirm deletion.
    printf("Delete user '%s'?\n", username);
    printf("  Repo directory : %s/%s\n", cfg.repoRoot, user.userDir);
    printf("  WARNING: this removes the user from the database only.\n");
    printf("           Repo files are NOT deleted — remove manually if needed.\n");
    printf("  Type 'yes' to confirm: ");
    fflush(stdout);
    char ans[16] = "";
    if (fgets(ans, sizeof(ans), stdin) == NULL || strncmp(ans, "yes", 3) != 0) {
        printf("Aborted.\n");
        return 0;
    }

    // Rewrite users.db omitting this user.
    char tmp[VCD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.userDb);
    FILE *in  = fopen(cfg.userDb, "r");
    FILE *out = fopen(tmp, "w");
    if (!in || !out) {
        fprintf(stderr, "vcd: cannot update userDb: %s\n", strerror(errno));
        if (in)  fclose(in);
        if (out) fclose(out);
        return 1;
    }

    char line[512];
    bool removed = false;
    while (fgets(line, sizeof(line), in)) {
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", line);
        char *tok = strtok(copy, ":");
        if (tok && strcmp(tok, username) == 0) {
            removed = true;   // skip this line
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);

    if (!removed) {
        remove(tmp);
        fprintf(stderr, "vcd: user '%s' not found in db\n", username);
        return 1;
    }

    if (rename(tmp, cfg.userDb) != 0) {
        fprintf(stderr, "vcd: rename failed: %s\n", strerror(errno));
        return 1;
    }

    printf("User '%s' deleted from %s\n", username, cfg.userDb);
    printf("Repo files remain at: %s/%s\n", cfg.repoRoot, user.userDir);
    printf("To remove repo files: rm -rf %s/%s\n", cfg.repoRoot, user.userDir);
    return 0;
}

static int cmd_listusers(void) {
    FILE *f = fopen(cfg.userDb, "r");
    if (!f) {
        fprintf(stderr, "vcd: cannot open userDb: %s\n", strerror(errno));
        return 1;
    }
    printf("%-20s  %s\n", "Username", "Repo directory");
    printf("%-20s  %s\n", "--------------------", "-----------------------------");
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char copy[512]; snprintf(copy, sizeof(copy), "%s", line);
        char *uname = strtok(copy, ":");
        strtok(NULL, ":"); // skip hash
        char *udir = strtok(NULL, ":");
        if (!uname) continue;
        if (udir) {
            size_t l = strlen(udir);
            while (l > 0 && (udir[l-1]=='\n'||udir[l-1]=='\r'||udir[l-1]==' '))
                udir[--l] = '\0';
        }
        char fullDir[VCD_MAX_PATH];
        snprintf(fullDir, sizeof(fullDir), "%s/%s",
                 cfg.repoRoot, udir ? udir : uname);
        printf("  %-18s  %s\n", uname, fullDir);
    }
    fclose(f);
    return 0;
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------
static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == EACCES || errno == EPERM) {
            fprintf(stderr, "vcd: cannot read config '%s': %s\n"
                            "     sudo chmod 644 %s\n",
                    path, strerror(errno), path);
            return 1;
        }
        return -1; // not found — ok on first run
    }

    // Track which fields were explicitly set in the config file.
    bool userDbSet = false, logFileSet = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        while (*key == ' ') key++;
        while (*val == ' ') val++;
        size_t kl = strlen(key), vl = strlen(val);
        while (kl > 0 && (key[kl-1]==' '||key[kl-1]=='\n')) key[--kl]='\0';
        while (vl > 0 && (val[vl-1]==' '||val[vl-1]=='\n')) val[--vl]='\0';
        if      (strcmp(key, "port")     == 0) cfg.port = atoi(val);
        else if (strcmp(key, "repoRoot") == 0) snprintf(cfg.repoRoot, sizeof(cfg.repoRoot), "%s", val);
        else if (strcmp(key, "userDb")   == 0) { snprintf(cfg.userDb, sizeof(cfg.userDb), "%s", val); userDbSet = true; }
        else if (strcmp(key, "logFile")  == 0) { snprintf(cfg.logFile, sizeof(cfg.logFile), "%s", val); logFileSet = true; }
        else if (strcmp(key, "runUser")  == 0) snprintf(cfg.runUser,  sizeof(cfg.runUser),  "%s", val);
    }
    fclose(f);

    // Derive userDb and logFile from repoRoot if not explicitly set.
    // This ensures they always live alongside the repo root regardless
    // of what the default compile-time paths were.
    if (!userDbSet && cfg.repoRoot[0])
        snprintf(cfg.userDb,  sizeof(cfg.userDb),  "%s/users.db", cfg.repoRoot);
    if (!logFileSet && cfg.repoRoot[0])
        snprintf(cfg.logFile, sizeof(cfg.logFile),  "%s/vcd.log",  cfg.repoRoot);

    return 0;
}

// ---------------------------------------------------------------------------
// Server loop
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// drop_privileges  –  switch from root to cfg.runUser after binding the port.
// Must be called after bind/listen and before accept.
// ---------------------------------------------------------------------------
static int drop_privileges(void) {
    if (getuid() != 0) return 0;  // not root — nothing to do

    if (cfg.runUser[0] == '\0' || strcmp(cfg.runUser, "root") == 0) {
        fprintf(stderr, "vcd: WARNING — running as root. "
                        "Set 'runUser' in vcd.conf to drop privileges.\n");
        vlog("WARNING: running as root\n");
        return 0;
    }

    struct passwd *pw = getpwnam(cfg.runUser);
    if (!pw) {
        fprintf(stderr, "vcd: runUser '%s' not found — cannot drop privileges.\n",
                cfg.runUser);
        return -1;
    }

    // Fix ownership of repoRoot and userDb so the new user can access them.
    // repoRoot needs read/write/execute; userDb needs read.
    if (chown(cfg.repoRoot, pw->pw_uid, pw->pw_gid) != 0)
        vlog("WARNING: cannot chown repoRoot %s: %s\n",
             cfg.repoRoot, strerror(errno));

    if (chown(cfg.userDb, pw->pw_uid, pw->pw_gid) != 0) {
        vlog("WARNING: cannot chown userDb %s: %s — making world-readable\n",
             cfg.userDb, strerror(errno));
        // Fallback: world-readable is acceptable since userDb contains
        // only crypt() hashes, not plaintext passwords.
        chmod(cfg.userDb, 0644);
    }

    // chown the log file too so the daemon can keep writing after drop.
    if (chown(cfg.logFile, pw->pw_uid, pw->pw_gid) != 0)
        vlog("WARNING: cannot chown logFile %s: %s\n",
             cfg.logFile, strerror(errno));

    // Set supplemental groups, then gid, then uid — order matters.
    if (initgroups(pw->pw_name, pw->pw_gid) != 0) {
        perror("vcd: initgroups"); return -1;
    }
    if (setgid(pw->pw_gid) != 0) {
        perror("vcd: setgid"); return -1;
    }
    if (setuid(pw->pw_uid) != 0) {
        perror("vcd: setuid"); return -1;
    }

    // Verify we cannot regain root.
    if (setuid(0) == 0) {
        fprintf(stderr, "vcd: FATAL — could not drop root privileges.\n");
        return -1;
    }

    printf("  Running as   : %s (uid=%d gid=%d)\n",
           pw->pw_name, (int)pw->pw_uid, (int)pw->pw_gid);
    vlog("Dropped privileges to %s uid=%d gid=%d\n",
         pw->pw_name, (int)pw->pw_uid, (int)pw->pw_gid);
    return 0;
}

static int cmd_start(void) {
    struct stat st;
    if (stat(cfg.repoRoot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "vcd: repoRoot '%s' missing or not a directory.\n"
                        "     Run: sudo vcd --init --reporoot %s\n",
                cfg.repoRoot, cfg.repoRoot); return 1;
    }
    if (access(cfg.userDb, R_OK) != 0) {
        fprintf(stderr, "vcd: cannot read userDb '%s': %s\n"
                        "     Run: sudo vcd --init\n",
                cfg.userDb, strerror(errno)); return 1;
    }

    logFp = fopen(cfg.logFile, "a");
    if (!logFp) {
        fprintf(stderr, "vcd: cannot open log '%s': %s — logging to stderr\n",
                cfg.logFile, strerror(errno));
        logFp = stderr;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) { perror("vcd: socket"); return 1; }
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)cfg.port);

    if (bind(serverFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("vcd: bind"); return 1;
    }
    if (listen(serverFd, VCD_BACKLOG) < 0) {
        perror("vcd: listen"); return 1;
    }


    printf("vcd v%s starting  (built %s %s)\n", VCD_VERSION, VC_BUILD_DATE, VC_BUILD_TIME);
    printf("  Port      : %d\n", cfg.port);
    printf("  Repo root : %s\n", cfg.repoRoot);
    printf("  User DB   : %s\n", cfg.userDb);
    printf("  Log       : %s\n", cfg.logFile);

    if (drop_privileges() != 0)
        return 1;

    printf("Listening...\n");
    vlog("vcd v%s started on port %d repoRoot=%s\n",
         VCD_VERSION, cfg.port, cfg.repoRoot);

    while (1) {
        struct sockaddr_in ca; socklen_t cal = sizeof(ca);
        int cfd = accept(serverFd, (struct sockaddr *)&ca, &cal);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        char addrStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ca.sin_addr, addrStr, sizeof(addrStr));
        vlog("Connect: %s (vcd v%s db=%s)\n", addrStr, APP_VERSION, cfg.userDb);

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); }
        else if (pid == 0) { close(serverFd); handle_client(cfd, addrStr); exit(0); }
        else { close(cfd); while (waitpid(-1, NULL, WNOHANG) > 0) {} }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static void usage(const char *prog) {
    printf("vcd — vc daemon v%s  (built %s %s)\n\n", VCD_VERSION, VC_BUILD_DATE, VC_BUILD_TIME);
    printf("Usage:\n");
    printf("  %s --init [--reporoot PATH]    Initialise config and directories\n", prog);
    printf("  %s --start [--port N]          Start the daemon\n", prog);
    printf("  %s --adduser <name>            Add a user\n", prog);
    printf("  %s --passwd  <name>            Change a user's password\n", prog);
    printf("  %s --deleteuser <name>         Delete a user (keeps repo files)\n", prog);
    printf("  %s --listusers                 List all users\n", prog);
    printf("  %s --testhash <password>       Show SHA-256 and crypt hash (diagnostic)\n", prog);
    printf("  %s --sethash <user> <sha256>  Write SHA-256 hash directly to db (diagnostic)\n", prog);
    printf("  %s --initrepo <path>           Create a repo (e.g. users/kelly/ushell)\n", prog);
    printf("  %s --reporoot <path>           Override repo root\n", prog);
    printf("  %s --port <n>                  Override port (default %d)\n",
           prog, VCD_DEFAULT_PORT);
    printf("  %s --conf <file>               Use alternate config file\n", prog);
    printf("  %s --version                   Print version\n", prog);
    printf("\nServer layout under repoRoot:\n");
    printf("  users/<username>/<reponame>/   personal repos\n");
    printf("  shared/<reponame>/             shared repos\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 0; }

    // Load config (non-fatal if missing).
    { char cp[VCD_MAX_PATH];
      snprintf(cp, sizeof(cp), "%s/vcd.conf", cfg.repoRoot);
      int lcr = load_config(cp);
      if (lcr == 1) return 1; }

    const char *action = NULL, *actionArg = NULL, *actionArg2 = NULL;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--init")      == 0) action = "init";
        else if (strcmp(argv[i], "--start")     == 0) action = "start";
        else if (strcmp(argv[i], "--listusers") == 0) action = "listusers";
        else if (strcmp(argv[i], "--initrepo") == 0 && i+1 < argc)
            { action = "initrepo"; actionArg = argv[++i]; }
        else if (strcmp(argv[i], "--initrepo") == 0) {
            fprintf(stderr, "vcd: --initrepo requires a path\n"
                            "     Example: vcd --initrepo users/kelly/ushell\n");
            return 1;
        }
        else if (strcmp(argv[i], "--version")   == 0) {
            printf("vcd version %s  (built %s %s)\n",
                   VCD_VERSION, VC_BUILD_DATE, VC_BUILD_TIME); return 0;
        }
        else if (strcmp(argv[i], "--help")      == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--adduser") == 0 && i+1 < argc)
            { action = "adduser"; actionArg = argv[++i]; }
        else if (strcmp(argv[i], "--passwd")     == 0 && i+1 < argc)
            { action = "passwd";      actionArg = argv[++i]; }
        else if (strcmp(argv[i], "--deleteuser") == 0 && i+1 < argc)
            { action = "deleteuser";  actionArg = argv[++i]; }
        else if (strcmp(argv[i], "--testhash") == 0 && i+1 < argc)
            { action = "testhash"; actionArg = argv[++i]; }
        else if (strcmp(argv[i], "--sethash") == 0 && i+2 < argc)
            { action = "sethash"; actionArg = argv[++i]; actionArg2 = argv[++i]; }
        else if (strcmp(argv[i], "--passwd")  == 0) {
            fprintf(stderr, "vcd: --passwd requires a username\n"); return 1;
        }
        else if (strcmp(argv[i], "--port")     == 0 && i+1 < argc)
            cfg.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--reporoot") == 0 && i+1 < argc)
            snprintf(cfg.repoRoot, sizeof(cfg.repoRoot), "%s", argv[++i]);
        else if (strcmp(argv[i], "--conf")     == 0 && i+1 < argc) {
            if (load_config(argv[++i]) == 1) return 1;
        }
        else {
            fprintf(stderr, "vcd: unknown option '%s'\n"
                            "     Run '%s --help'\n", argv[i], argv[0]);
            return 1;
        }
    }

    if (!action) { usage(argv[0]); return 0; }

    if      (strcmp(action, "init")      == 0) return cmd_init();
    else if (strcmp(action, "start")     == 0) return cmd_start();
    else if (strcmp(action, "adduser")    == 0) return cmd_adduser(actionArg);
    else if (strcmp(action, "passwd")     == 0) return cmd_passwd(actionArg);
    else if (strcmp(action, "deleteuser") == 0) return cmd_deleteuser(actionArg);
    else if (strcmp(action, "listusers")  == 0) return cmd_listusers();
    else if (strcmp(action, "testhash")  == 0) {
        if (!actionArg) {
            fprintf(stderr, "Usage: sudo vcd --testhash <password>\n");
            return 1;
        }
        printf("Config:\n");
        printf("  userDb   : %s\n", cfg.userDb);
        printf("  repoRoot : %s\n\n", cfg.repoRoot);
        char sha[65];
        sha256hex_server(actionArg, sha);
        printf("SHA-256 (sent by client) : %s\n", sha);
        char stored_hash[256];
        hash_password(actionArg, stored_hash, sizeof(stored_hash));
        printf("crypt   (would be stored): %s\n", stored_hash);
        printf("\nTo set this password: sudo vcd --passwd %s\n",
               actionArg ? "<username>" : "");
        printf("To verify DB:          sudo cat %s\n", cfg.userDb);
        return 0;
    }
    else if (strcmp(action, "sethash") == 0) {
        // --sethash <user> <sha256hex>
        // Directly stores crypt(sha256hex) for a user — for debugging.
        // Usage: sudo vcd --sethash kelly 67d6313c7e5a09ba...
        if (!actionArg || !actionArg2) {
            fprintf(stderr, "Usage: sudo vcd --sethash <username> <sha256hex>\n");
            return 1;
        }
        VcdUser user;
        if (!db_load_user(actionArg, &user)) {
            fprintf(stderr, "vcd: user '%s' not found in %s\n",
                    actionArg, cfg.userDb);
            return 1;
        }
        // Hash the provided sha256hex with crypt for storage.
        char salt[32] = "$6$";
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        const char chars[] =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
        for (int i = 3; i < 19; i++)
            salt[i] = chars[rand() % (sizeof(chars)-1)];
        salt[19] = '$'; salt[20] = '\0';
        struct crypt_data cd = {0};
        char *result = crypt_r(actionArg2, salt, &cd);
        if (!result) {
            fprintf(stderr, "vcd: crypt failed\n"); return 1;
        }
        // Rewrite db.
        char tmp[VCD_MAX_PATH];
        snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.userDb);
        FILE *in = fopen(cfg.userDb, "r"), *out = fopen(tmp, "w");
        if (!in || !out) {
            fprintf(stderr, "vcd: cannot open userDb %s\n", cfg.userDb);
            if (in) fclose(in); if (out) fclose(out); return 1;
        }
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            char copy[512];
            snprintf(copy, sizeof(copy), "%s", line);
            char *tok = strtok(copy, ":");
            if (tok && strcmp(tok, actionArg) == 0)
                fprintf(out, "%s:%s:%s\n", actionArg, result, user.userDir);
            else
                fputs(line, out);
        }
        fclose(in); fclose(out);
        rename(tmp, cfg.userDb);
        printf("Hash updated for '%s' in %s\n", actionArg, cfg.userDb);
        printf("Stored: %s\n", result);
        return 0;
    }
    else if (strcmp(action, "initrepo")  == 0) return cmd_initrepo(actionArg);

    usage(argv[0]); return 1;
}
