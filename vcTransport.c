// vcTransport.c  –  client transport library for vcd protocol.

#include <stdio.h>
#include "vcPlatform.h"
#include "vcSha256.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include "vc.h"
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef VC_WINDOWS
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   // Winsock uses closesocket() not close()
#  define close(s) closesocket(s)
   // Winsock uses send/recv not write/read for sockets
#  define write(s,b,n) send(s,(const char*)(b),(int)(n),0)
#  define read(s,b,n)  recv(s,(char*)(b),(int)(n),0)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

#include "vcd.h"
#include "vcTransport.h"

// ---------------------------------------------------------------------------
// SHA-256 password hashing (via sha256sum system command).
// The plaintext password never travels the wire — only its SHA-256 hex.
// ---------------------------------------------------------------------------
static void sha256hex(const char *input, char out[65]) {
    vc_sha256_hex(input, out);
}

// ---------------------------------------------------------------------------
// Low-level I/O helpers.
// ---------------------------------------------------------------------------
static bool send_line(int fd, const char *fmt, ...) {
    char buf[VCD_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[n]   = '\n';
    buf[n+1] = '\0';
    return write(fd, buf, (size_t)(n + 1)) == n + 1;
}

static bool recv_line(int fd, char *buf, size_t sz) {
    size_t n = 0;
    while (n < sz - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return n > 0;
}

static bool is_ok(const char *line) {
    return strncmp(line, RESP_OK, 2) == 0;
}

static bool send_data(int fd, const void *data, size_t sz) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < sz) {
        ssize_t n = write(fd, p + sent, sz - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recv_data(int fd, void *buf, size_t sz) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, p + got, sz - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// vct_connect
// ---------------------------------------------------------------------------
bool vct_connect(VcSession *s, const char *host, int port,
                 const char *username, const char *password) {
#ifdef VC_WINDOWS
    // Initialise Winsock on first call.
    static int wsaInit = 0;
    if (!wsaInit) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2,2), &wd);
        wsaInit = 1;
    }
#endif
    memset(s, 0, sizeof(*s));
    snprintf(s->host,     sizeof(s->host),     "%s", host);
    snprintf(s->username, sizeof(s->username), "%s", username);
    s->port = port;
    s->fd   = -1;

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "vc: cannot resolve '%s'\n", host);
        return false;
    }

    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) { perror("vc: socket"); return false; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);

    if (connect(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ECONNREFUSED)
            fprintf(stderr, "vc: vcd is not running on %s:%d\n"
                            "     Start it with: sudo vcd --start\n",
                    host, port);
        else
            fprintf(stderr, "vc: cannot connect to %s:%d — %s\n",
                    host, port, strerror(errno));
        close(s->fd); s->fd = -1;
        return false;
    }

    char line[VCD_MAX_LINE];

    // Receive server greeting.
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: unexpected greeting: %s\n", line);
        goto fail;
    }

    // Send HELLO.
    send_line(s->fd, "%s %s", CMD_HELLO, VCD_VERSION);
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: HELLO rejected: %s\n", line);
        goto fail;
    }

    // Send AUTH with SHA-256 hashed password.
    {
        char hash[65];
        sha256hex(password, hash);
        if (hash[0] == '\0') {
            // sha256sum not available — send empty hash, server will reject.
            fprintf(stderr, "vc: sha256sum not found on this system.\n");
            goto fail;
        }
        send_line(s->fd, "%s %s %s", CMD_AUTH, username, hash);
    }
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: authentication failed: %s\n", line);
        goto fail;
    }

    s->authenticated = true;
    return true;

fail:
    close(s->fd); s->fd = -1;
    return false;
}

// ---------------------------------------------------------------------------
// vct_disconnect
// ---------------------------------------------------------------------------
void vct_disconnect(VcSession *s) {
    if (s->fd >= 0) {
        send_line(s->fd, "%s", CMD_QUIT);
        close(s->fd);
        s->fd = -1;
    }
    s->authenticated = false;
}

// ---------------------------------------------------------------------------
// vct_push
// ---------------------------------------------------------------------------
bool vct_push(VcSession *s, const char *repoPath,
              const char *localPath, const char *relPath) {
    struct stat st;
    if (stat(localPath, &st) != 0) {
        fprintf(stderr, "vc: local file not found: %s\n", localPath);
        return false;
    }

    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s %s %lld",
              CMD_PUSH, repoPath, relPath, (long long)st.st_size);

    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: PUSH rejected for %s: %s\n", relPath, line);
        return false;
    }

    // Send file data.
    FILE *f = fopen(localPath, "rb");
    if (!f) {
        fprintf(stderr, "vc: cannot open %s: %s\n",
                localPath, strerror(errno));
        return false;
    }
    char buf[VCD_TRANSFER_BUF];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (!send_data(s->fd, buf, n)) { ok = false; break; }
    }
    fclose(f);
    if (!ok) return false;

    // Wait for server confirmation.
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: PUSH transfer failed for %s: %s\n", relPath, line);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// vct_pull
// ---------------------------------------------------------------------------
bool vct_pull(VcSession *s, const char *repoPath,
              const char *relPath, const char *localPath,
              bool skip_if_same_size) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s %s", CMD_PULL, repoPath, relPath);

    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        return false;
    }

    long long remoteSize = 0;
    sscanf(line + 3, "%lld", &remoteSize);

    // Optionally skip immutable zips already present at correct size.
    if (skip_if_same_size) {
        struct stat lst;
        if (stat(localPath, &lst) == 0 &&
            (long long)lst.st_size == remoteSize) {
            // Drain the incoming data so the server stays in sync.
            long long drained = 0;
            char discard[VCD_TRANSFER_BUF];
            while (drained < remoteSize) {
                size_t chunk = (size_t)((remoteSize - drained) > (long long)sizeof(discard)
                               ? (long long)sizeof(discard) : (remoteSize - drained));
                if (!recv_data(s->fd, discard, chunk)) break;
                drained += (long long)chunk;
            }
            return true;
        }
    }

    // Ensure parent directory exists using mkdir -p logic.
    char parent[VCD_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", localPath);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        // Create each component of the path.
        for (char *p = parent + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(parent, 0755);
                *p = '/';
            }
        }
        mkdir(parent, 0755);
    }

    // Write to temp file, rename atomically on success.
    char tmp[VCD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", localPath);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "vc: cannot create %s: %s (localPath=%s)\n",
                tmp, strerror(errno), localPath);
        // Drain incoming data to keep connection in sync.
        char discard[VCD_TRANSFER_BUF];
        long long drain = remoteSize;
        while (drain > 0) {
            size_t chunk = (size_t)(drain > (long long)sizeof(discard)
                           ? (long long)sizeof(discard) : drain);
            if (!recv_data(s->fd, discard, chunk)) break;
            drain -= (long long)chunk;
        }
        return false;
    }

    char buf[VCD_TRANSFER_BUF];
    long long remaining = remoteSize;
    bool ok = true;
    while (remaining > 0) {
        size_t chunk = (size_t)(remaining > (long long)sizeof(buf)
                       ? (long long)sizeof(buf) : remaining);
        if (!recv_data(s->fd, buf, chunk)) { ok = false; break; }
        if (fwrite(buf, 1, chunk, f) != chunk) { ok = false; break; }
        remaining -= (long long)chunk;
    }
    fclose(f);

    if (!ok || rename(tmp, localPath) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// vct_list
// ---------------------------------------------------------------------------
// vct_repo_exists — silently check if a repo exists on the server.
bool vct_repo_exists(VcSession *s, const char *repoPath) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s", CMD_LIST, repoPath);
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line))
        return false;
    // Drain the file list.
    while (recv_line(s->fd, line, sizeof(line)))
        if (strcmp(line, "END") == 0) break;
    return true;
}

bool vct_list(VcSession *s, const char *repoPath,
              void (*cb)(const char *relPath, long long size, void *ud),
              void *userdata, int *out_count) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s", CMD_LIST, repoPath);

    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: LIST failed: %s\n", line);
        return false;
    }

    // Parse count from "OK N files".
    int count = 0;
    sscanf(line + 3, "%d", &count);
    if (out_count) *out_count = count;

    while (recv_line(s->fd, line, sizeof(line))) {
        if (strcmp(line, "END") == 0) break;
        char relPath[VCD_MAX_PATH];
        long long size = 0;
        if (sscanf(line, "%511s %lld", relPath, &size) >= 1 && cb)
            cb(relPath, size, userdata);
    }
    return true;
}

// ---------------------------------------------------------------------------
// vct_list_repos
// ---------------------------------------------------------------------------
bool vct_list_repos(VcSession *s,
                    void (*cb)(const char *repoPath, const char *tag, void *ud),
                    void *userdata) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s", CMD_LISTREPOS);

    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: LISTREPOS failed: %s\n", line);
        return false;
    }

    while (recv_line(s->fd, line, sizeof(line))) {
        if (strcmp(line, "END") == 0) break;
        // Server sends: "repopath [tag]"
        // Split on the last space before '['
        char path[VCD_MAX_LINE], tag[64] = "";
        char *bracket = strrchr(line, '[');
        if (bracket && bracket > line) {
            size_t plen = (size_t)(bracket - line);
            while (plen > 0 && line[plen-1] == ' ') plen--;
            snprintf(path, sizeof(path), "%.*s", (int)plen, line);
            snprintf(tag,  sizeof(tag),  "%.*s",
                     (int)(strlen(bracket)-2), bracket+1);
        } else {
            snprintf(path, sizeof(path), "%s", line);
        }
        if (cb) cb(path, tag, userdata);
    }
    return true;
}

// vct_initrepo — ask the server to create the repo if it doesn't exist.
// Returns true if created or already existed.
bool vct_initrepo(VcSession *s, const char *repoPath) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s", CMD_INITREPO, repoPath);
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: INITREPO failed: %s\n", line);
        return false;
    }
    return true;
}

// Collect all LIST entries into a heap array — fully drains the LIST
// response before the caller issues any PULL commands.
int vct_list_collect(VcSession *s, const char *repoPath,
                     VctFileEntry **entries) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s", CMD_LIST, repoPath);

    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: LIST failed: %s\n", line);
        *entries = NULL;
        return -1;
    }

    int count = 0;
    sscanf(line + 3, "%d", &count);

    // Allocate — add 1 so count=0 still gives a valid pointer.
    VctFileEntry *arr = calloc((size_t)(count + 1), sizeof(VctFileEntry));
    if (!arr) { *entries = NULL; return -1; }

    int n = 0;
    while (recv_line(s->fd, line, sizeof(line))) {
        if (strcmp(line, "END") == 0) break;
        if (n >= count + 64) break;  // safety: never exceed count+headroom
        char path[VCD_MAX_PATH];
        long long sz = 0;
        if (sscanf(line, "%511s %lld", path, &sz) >= 1) {
            snprintf(arr[n].relPath, 512, "%s", path);
            arr[n].size = sz;
            n++;
        }
    }

    *entries = arr;
    return n;
}

// vct_moverepo — ask the server to move a repo from srcPath to dstPath.
// Returns true on success; newPath receives the confirmed destination.
bool vct_moverepo(VcSession *s, const char *srcPath, const char *dstPath,
                  char *newPath, size_t sz) {
    char line[VCD_MAX_LINE];
    send_line(s->fd, "%s %s %s", CMD_MOVEREPO, srcPath, dstPath);
    if (!recv_line(s->fd, line, sizeof(line)) || !is_ok(line)) {
        fprintf(stderr, "vc: MOVEREPO failed: %s\n", line);
        return false;
    }
    // Response: "OK <newpath>"
    if (newPath && sz > 0)
        snprintf(newPath, sz, "%s", line + 3);
    return true;
}

// vct_remote_status — compare local .vc/data/ zips against remote.
// Returns: remote_count - local_count (positive = remote ahead,
//          negative = local ahead, 0 = in sync, -999 on error).
int vct_remote_status(VcSession *s, const char *repoPath,
                      const char *localDataDir,
                      int *remoteOnly, int *localOnly) {
    *remoteOnly = 0;
    *localOnly  = 0;

    // Collect remote file list.
    VctFileEntry *files = NULL;
    int total = vct_list_collect(s, repoPath, &files);
    if (total < 0) return -999;

    // Build a set of remote zip names (data/*.zip only).
    // Count how many remote zips the local data dir doesn't have.
    int remoteZips = 0;
    for (int i = 0; i < total; i++) {
        const char *rel = files[i].relPath;
        // Only count .vc/data/*.zip entries.
        const char *dataPrefix = ".vc/data/";
        size_t dpLen = strlen(dataPrefix);
        if (strncmp(rel, dataPrefix, dpLen) != 0) continue;
        const char *zipName = rel + dpLen;
        size_t zl = strlen(zipName);
        if (zl < 4 || strcmp(zipName + zl - 4, ".zip") != 0) continue;

        remoteZips++;

        // Check if local has this zip.
        char localPath[MAX_DIR_PATH];
        snprintf(localPath, sizeof(localPath), "%s/%s", localDataDir, zipName);
        struct stat st;
        if (stat(localPath, &st) != 0)
            (*remoteOnly)++;
    }

    // Count local zips not on remote.
    DIR *d = opendir(localDataDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t nl = strlen(de->d_name);
            if (nl < 4 || strcmp(de->d_name + nl - 4, ".zip") != 0) continue;
            // Check if remote has this zip.
            bool found = false;
            for (int i = 0; i < total; i++) {
                const char *rel = files[i].relPath;
                const char *dataPrefix = ".vc/data/";
                if (strncmp(rel, dataPrefix, strlen(dataPrefix)) != 0) continue;
                if (strcmp(rel + strlen(dataPrefix), de->d_name) == 0) {
                    found = true; break;
                }
            }
            if (!found) (*localOnly)++;
        }
        closedir(d);
    }

    free(files);
    return remoteZips;
}
