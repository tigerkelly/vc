#ifndef VC_TRANSPORT_H
#define VC_TRANSPORT_H

// ---------------------------------------------------------------------------
// vcTransport.h  –  client-side transport library for talking to vcd.
//
// Usage:
//   VcSession s;
//   if (!vct_connect(&s, host, port, username, password)) { ... }
//   vct_push(&s, repoPath, localFile, remoteRelPath);
//   vct_pull(&s, repoPath, remoteRelPath, localFile);
//   vct_disconnect(&s);
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include "vcd.h"

typedef struct {
    int  fd;
    char host[256];
    int  port;
    char username[64];
    bool authenticated;
} VcSession;

// Connect and authenticate. Returns true on success.
bool vct_connect(VcSession *s, const char *host, int port,
                 const char *username, const char *password);

// Disconnect cleanly.
void vct_disconnect(VcSession *s);

// Push a local file to repoPath/relPath on the server.
bool vct_push(VcSession *s, const char *repoPath,
              const char *localPath, const char *relPath);

// Pull repoPath/relPath from server to localPath.
// If skip_if_same_size is true, skips download when local file
// already exists at the same size (immutable commit zips).
bool vct_pull(VcSession *s, const char *repoPath,
              const char *relPath, const char *localPath,
              bool skip_if_same_size);

// List files in a repo. Calls cb(relpath, size, userdata) for each.
// If out_count is non-NULL, receives the total file count from the server.
bool vct_list(VcSession *s, const char *repoPath,
              void (*cb)(const char *relPath, long long size, void *ud),
              void *userdata, int *out_count);

// List all repos the authenticated user can access.
// Calls cb(repoPath, userdata) for each repo.
bool vct_list_repos(VcSession *s,
                    void (*cb)(const char *repoPath, const char *tag, void *ud),
                    void *userdata);

#endif // VC_TRANSPORT_H

// Ask the server to create the repo if it doesn't already exist.
// Returns true if created or already existed.
bool vct_initrepo(VcSession *s, const char *repoPath);

// Silently check if a repo exists on the server (no error messages).
bool vct_repo_exists(VcSession *s, const char *repoPath);

// FileEntry — one item from a LIST response.
typedef struct {
    char      relPath[512];
    long long size;
} VctFileEntry;

// Collect all files from a LIST into a heap-allocated array.
// Caller must free(*entries) when done.
// Returns number of entries, or -1 on error.
int vct_list_collect(VcSession *s, const char *repoPath,
                     VctFileEntry **entries);

// Move a repo on the server (e.g. users/kelly/app → shared/app).
// newPath receives the confirmed destination path.
bool vct_moverepo(VcSession *s, const char *srcPath, const char *dstPath,
                  char *newPath, size_t sz);

// Compare local .vc/data/ against remote.
// remoteOnly = commits on remote not in local (pull needed)
// localOnly  = commits in local not on remote (push needed)
// Returns total remote zip count, or -999 on error.
int vct_remote_status(VcSession *s, const char *repoPath,
                      const char *localDataDir,
                      int *remoteOnly, int *localOnly);
