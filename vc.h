#ifndef _VC_H
#define _VC_H

#include "vcPlatform.h"

#include <dirent.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>

#define VC_DIR		".vc"
#define VC_DATA_DIR	"data"
#define VC_FILE		"file.vc"
#define VC_CFGFILE	".vc/config.vc"
#define VC_IGNORE	".vcignore"

#define MAX_USERNAME		256
#define MAX_HOST			256
#define MAX_EMAIL			256
#define MAX_REPO			256
#define SALT_SIZE			32
#define MAX_PASSWD_SIZE		64
#define MAX_USER_NAME		64
#define MAX_PROJECT_NAME	64
#define MAX_HOST_NAME		64
#define MAX_REPO_NAME		256
#define MAX_DIR_PATH		2048

#define MAX_IGNORE		256

typedef struct _cmdargs {
    char *name;
    bool args;
    char *desc;
} Cmds;

typedef struct _config {
	char user[MAX_USERNAME];
	char loginName[MAX_USERNAME];  // legacy — migrated to vcdUser on load
	char vcdUser[MAX_USERNAME];    // vcd daemon username
	char owner[MAX_USERNAME];      // login name of the user who ran 'vc init'
	char host[MAX_HOST];
	char email[MAX_EMAIL];
	char project[MAX_PROJECT_NAME];
	char repo[MAX_REPO];
	char allowedUsers[1024];       // comma-separated vcd usernames for private repos
	bool isPrivate;                // if true, only owner may push/pull
	int  port;                     // vcd daemon port (default 9876)
} Config;

typedef enum {
	VC_INIT,
	VC_LOG,
	VC_ADD,
	VC_COPY,
	VC_BRANCH,
	VC_STATUS,
	VC_TAG,
	VC_COMMIT,
	VC_VERSION,
	VC_HELP,
	VC_DIFF,
	VC_ARCHIVE,
	VC_RENAME,
	VC_REVERT,
	VC_CONFIG,
	VC_PUSH,
	VC_PULL,
	VC_UNKNOWN
} Action;

typedef struct _ignore {
	char *pattern;
} Ignore;

// -------------------------------------------------------------------
// Index – tracks every file vc knows about.
// Lives at .vc/index, one entry per line: path|state|mtime
// -------------------------------------------------------------------
typedef enum {
	INDEX_STAGED,
	INDEX_COMMITTED,
	INDEX_MODIFIED
} IndexState;

typedef struct {
	char       *path;   // relative to vcTopDir
	IndexState  state;
	time_t      mtime;
	off_t       size;   // file size in bytes — used with mtime for change detection
} IndexEntry;

extern int          vcIndexLoad(IndexEntry **entriesOut);
extern int          vcIndexSave(IndexEntry *entries, int count);
extern void         vcIndexFree(IndexEntry *entries, int count);
extern int          vcIndexFind(IndexEntry *entries, int count, const char *relPath);
extern IndexEntry  *vcIndexStage(IndexEntry *entries, int *count, const char *fullPath);
extern void         vcIndexMarkCommitted(IndexEntry *entries, int count);

typedef int (*vcAction)(char *fn);

// These functions are passed to the vcWalk function.  The vcWalk function calls these actions.
extern int vcAddAction(char *path);
extern int vcCommitAction(char *path);
extern int vcDisplayAction(char *path);		// Just displays the path, for testing

extern char *vcTopDir;
extern char *vcStartDir;   // original cwd before chdir(vcTopDir)
extern bool useNewt;

extern Ignore ignores[MAX_IGNORE];

extern void printErr(char *msg, ...);
extern void vcPrintBanner();
extern void trim(char *str);
extern int  parse(char *str, const char *delimiters, char **args, int maxArgs);
extern void loadIgnores(void);
extern bool isDir(char *path);
extern int vcInit(int argc, char *argv[]);
extern int vcBranchEnsureInit(void);
extern char *vcBranchCurrentName(void);  // heap-allocated, caller must free
extern int vcBranchRecordCommit(const char *zipName);
extern bool vcRestoreFileFromZip(const char *zipPath, const char *relPath);
extern bool vcBranchCommitZip(char *buf, size_t sz);
extern int vcCheckout(int argc, char *argv[]);
extern int vcStatus(bool remoteCheck);
extern int vcAdd(int argc, char *argv[]);
extern int vcList(int argc, char *argv[]);
extern int vcDelete(int argc, char *argv[]);
extern int vcBranch(char *branchName);
extern int vcCommit(int argc, char *argv[]);
extern int vcCopy(char *path);
extern int vcConfig(Config *cfg);
extern int vcConfigShow(Config *cfg);
extern int vcConfigSet(Config *cfg, const char *key, const char *value);
extern int vcConfigSave(Config *cfg);
extern int vcInfo(void);
extern int vcShow(int argc, char *argv[]);
extern int vcIgnore(int argc, char *argv[]);
extern int vcStash(int argc, char *argv[]);
extern int vcHistory(int argc, char *argv[]);
extern int vcMerge(int argc, char *argv[]);
extern int vcTag(int argc, char *argv[]);
extern int vcTagEnsureInit(void);
extern int vcArchive(int argc, char *argv[]);
extern int vcDiff(int argc, char *argv[]);
extern int vcRename(char * oldName, char *newName);
extern int vcRevert(char *fileName);
extern int vcPush(int argc, char *argv[]);
extern int vcPull(int argc, char *argv[]);
extern void vcWalk(char *path, vcAction action);
extern int vcClone(int argc, char *argv[]);
extern int vcLog(char *msg, ...);
extern int vcShowLog();
extern char *vcSalt(int saltSize);
extern char *encryptString(char *password, char *salt);

extern int getRepo();
extern int getEmail();
extern int getProject();
extern int getUserName();
extern int getHostName();

extern char *cIgnore[];
extern char *cplusplusIgnore[];
extern char *javaIgnore[];
extern char *goIgnore[];
extern char *rustIgnore[];

extern char *repoPath;
extern char *userName;
extern char *prjName;
extern char *hostName;
extern char *email;
#endif
