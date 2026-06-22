#include <stdio.h>
#include "vcPlatform.h"
#include "vcVersion.h"
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <regex.h>
#include <dirent.h>


#include "vc.h"
#include "vcAuth.h"
#include "vcd.h"
#include "vcTransport.h"

char *prgVersion = "1.0.0";
Action action = VC_UNKNOWN;


char *cmds[] = {
	"version", "log", "help", "status", "add", "clone",
	"archive", "init", "branch", "commit", "tag", "delete",
	"diff", "revert", "config", "push", "pull", "checkout",
	"rename", "list", "info", "show", "ignore", "stash",
	"history", "move", "untrack", "merge", "auth", "moverepo", "repos",
	NULL
};

Ignore ignores[MAX_IGNORE];

regex_t preg;

char *tag = NULL;
char *branch = NULL;
char *delete = NULL;
char *diff = NULL;

// Version is defined in vcVersion.h as APP_VERSION
char *prgName = NULL;
Config *config = NULL;

unsigned short portNum = 0;

char *repoPath = NULL;
char *userName = NULL;
char *prjName = NULL;
char *hostName = NULL;
char *email = NULL;
char *command = NULL;
char *vcTopDir   = NULL;
char *vcStartDir = NULL;   // original cwd before chdir(vcTopDir)
char *vcDir = NULL;			// .vc dir path.

bool isRemote = false;

void help(char *name, int ext);
bool isDir(char *dirPath);
uid_t getOwner(char *owner);
gid_t getGroup(char *group);
void printVersion();
Config *loadConfig();
void loadIgnores();
void printConfig();
int isValidIpv4(const char *ip);
int isValidOctet(const char *s);
int getRepo();
int getEmail();
int getProject();
int getUserName();
int getHostName();
char *findProjectDir();


int main(int argc, char *argv[]) {

	vc_init_terminal();  // enable ANSI colour on Windows 10+


	char *p = strrchr(argv[0], '/');

	if (p != NULL) {
		prgName = strdup(++p);
	} else {
		prgName = argv[0];
	}

	if (argc < 2) {
		fprintf(stderr, "Missing command argument.\n");
		help(prgName, 1);
	}

	// Handle --version before any command validation or repo search.
	if (strcmp(argv[1], "--version") == 0) {
		printf("vc version %s  (built %s %s)\n", APP_VERSION, VC_BUILD_DATE, VC_BUILD_TIME);
		return 0;
	}

	command = strdup(argv[1]);

	bool invalid = true;

	int idx = 0;
	while (cmds[idx] != NULL) {
		if (strcasecmp(command, cmds[idx]) == 0) {
			invalid = false;
			break;
		}
		idx++;
	}

	if (invalid == true) {
		printErr("Unknown command '%s'\n", command);
		help(prgName, 2);
	}

	// find top directory of this repo.
	vcTopDir = findProjectDir();
	// if (vcTopDir == NULL) {
		// printErr("Could not find the .vc directory in directory path\n");
		// return -1;
	// }
	// printf("vcTopDir: '%s'\n", vcTopDir);

	// Capture the original cwd BEFORE changing to the project root.
	// Commands like 'branch' need to know if the user was in a subdirectory.
	{
		char buf[MAX_DIR_PATH];
		vcStartDir = (getcwd(buf, sizeof(buf)) != NULL) ? strdup(buf) : NULL;
	}

	// Change directory to the top directory.
	if (vcTopDir != NULL)
		chdir(vcTopDir);

	// Commands that do not need an existing repo.
	bool needsRepo = !(strcasecmp(command, "help")    == 0 ||
	                   strcasecmp(command, "version") == 0 ||
	                   strcasecmp(command, "init")    == 0 ||
	                   strcasecmp(command, "clone")   == 0 ||
	                   strcasecmp(command, "auth")    == 0 ||
	                   strcasecmp(command, "repos")   == 0);

	if (needsRepo && vcTopDir == NULL) {
		fprintf(stderr, "vc: not a vc repository.\n"
		                "    No .vc/ directory found here or in any parent directory.\n"
		                "    Run 'vc init' to create a new repository here.\n");
		exit(1);
	}

	loadIgnores();

	config = loadConfig();
	// printConfig();

	// vcLog("Starting vc...");

	if (config == NULL) {
		printErr("Could not read %s file.\n", VC_CFGFILE);
		exit(-1);
	}

	if (argc <= 1) {
		exit(0);
	}

	// Print branch/project banner before every command except help and version.
	if (strcasecmp(command, "help")    != 0 &&
	    strcasecmp(command, "version") != 0 &&
	    strcasecmp(command, "clone")   != 0 &&
	    strcasecmp(command, "auth")    != 0) {
		vcPrintBanner();
	}

	if (strcasecmp(command, "help") == 0) {
		help(prgName, 0);
	} else if (strcasecmp(command, "log") == 0) {
		FILE *f = fopen(".vc/vc.log", "r");
		if (f != NULL) {
			char line[256];
			printf("Log for %s.\n", vcTopDir);
			while (fgets(line, sizeof(line), f) != NULL) {
				printf("%s", line);
			}
			fclose(f);
		}
	} else if (strcasecmp(command, "info") == 0) {
		return vcInfo();
	} else if (strcasecmp(command, "show") == 0) {
		return vcShow(argc, argv);
	} else if (strcasecmp(command, "ignore") == 0) {
		return vcIgnore(argc, argv);
	} else if (strcasecmp(command, "stash") == 0) {
		return vcStash(argc, argv);
	} else if (strcasecmp(command, "history") == 0) {
		return vcHistory(argc, argv);
	} else if (strcasecmp(command, "move") == 0) {
		// Alias for rename.
		if (argc < 4) {
			printErr("No oldName and/or newName for move command.\n");
			help(prgName, -1);
		}
		return vcRename(argv[2], argv[3]);
	} else if (strcasecmp(command, "untrack") == 0) {
		// Alias for delete --keep.
		if (argc < 3) {
			printErr("No argument for untrack command.\n");
			help(prgName, -1);
		}
		char *untrackArgv[] = { argv[0], argv[1], "--keep", argv[2], NULL };
		return vcDelete(4, untrackArgv);
	} else if (strcasecmp(command, "merge") == 0) {
		return vcMerge(argc, argv);
	} else if (strcasecmp(command, "auth") == 0) {
		// vc auth --clear  → remove stored credentials
		// vc auth          → show whether credentials are stored
		extern char *vcTopDir;
		if (argc >= 3 && strcmp(argv[2], "--clear") == 0) {
			vcAuthClear(vcTopDir);
		} else {
			char buf[256] = "";
			extern Config *config;
			if (config && vcAuthLoad(vcTopDir,
			                         config->host,
			                         config->vcdUser,
			                         buf, sizeof(buf))) {
				printf("Stored credentials: vcd password for %s@%s is saved in .vc/auth\n",
				       config->vcdUser,
				       config->host);
				printf("Use 'vc auth --clear' to remove.\n");
			} else {
				printf("No stored credentials. Password will be prompted on push/pull.\n");
				printf("Use 'vc auth --save' or answer 'y' when prompted during push/pull.\n");
			}
			memset(buf, 0, sizeof(buf));
		}
		return 0;
	} else if (strcasecmp(command, "moverepo") == 0) {
		// vc moverepo <srcpath> <dstpath>
		// Moves a repo on the vcd server — e.g. from users/ to shared/ or back.
		// Also updates local config.vc repo field.
		if (argc < 4) {
			fprintf(stderr, "Usage: vc moverepo <srcpath> <dstpath>\n"
			                "Example:\n"
			                "  vc moverepo users/kelly/ushell shared/ushell\n"
			                "  vc moverepo shared/ushell users/kelly/ushell\n");
			return 1;
		}
		const char *srcPath = argv[2];
		const char *dstPath = argv[3];

		extern Config *config;
		if (!config || config->host[0] == '\0') {
			fprintf(stderr, "vc moverepo: no remote host configured.\n"
			                "             Run: vc config --set host <hostname>\n");
			return 1;
		}
		const char *vcdUser = config->vcdUser[0] ? config->vcdUser : config->loginName;
		int port = config->port > 0 ? config->port : VCD_DEFAULT_PORT;

		char password[256] = "";
		extern char *vcTopDir;
		if (!vcAuthPrompt(vcTopDir, config->host, vcdUser,
		                  password, sizeof(password))) {
			fprintf(stderr, "vc moverepo: no password provided.\n");
			return 1;
		}

		VcSession s;
		if (!vct_connect(&s, config->host, port, vcdUser, password)) {
			memset(password, 0, sizeof(password));
			return 1;
		}
		memset(password, 0, sizeof(password));

		char newPath[VCD_MAX_PATH];
		if (!vct_moverepo(&s, srcPath, dstPath, newPath, sizeof(newPath))) {
			vct_disconnect(&s);
			return 1;
		}
		vct_disconnect(&s);

		printf("Repo moved: %s → %s\n", srcPath, newPath);

		// Update local config.vc if it points to the source path.
		if (strcmp(config->repo, srcPath) == 0) {
			snprintf(config->repo, sizeof(config->repo), "%s", newPath);
			vcConfigSave(config);
			printf("Updated local config: repo = %s\n", newPath);
		} else {
			printf("Note: update your local config if needed:\n");
			printf("  vc config --set repo %s\n", newPath);
		}
		return 0;
	} else if (strcasecmp(command, "repos") == 0) {
		// vc repos <host> <username>
		// vc repos <host> <username> --port <port>
		// Lists all repos on the vcd server visible to this user.
		// Does not require a local .vc/ directory.
		if (argc < 4) {
			fprintf(stderr, "Usage: vc repos <host> <username> [--port <port>]\n");
			fprintf(stderr, "Example: vc repos 192.168.0.170 kelly\n");
			return 1;
		}
		const char *reposHost = argv[2];
		const char *reposUser = argv[3];
		int reposPort = VCD_DEFAULT_PORT;

		// Parse optional --port flag.
		for (int i = 4; i < argc - 1; i++) {
			if (strcmp(argv[i], "--port") == 0)
				reposPort = atoi(argv[i+1]);
		}

		// Prompt for password directly (no .vc/auth cache for standalone use).
		char password[256] = "";
		printf("vcd password for %s@%s: ", reposUser, reposHost);
		fflush(stdout);
		vc_echo_off();
		if (fgets(password, sizeof(password), stdin) != NULL) {
			size_t pl = strlen(password);
			if (pl > 0 && password[pl-1] == '\n') password[--pl] = '\0';
			if (pl > 0 && password[pl-1] == '\r') password[--pl] = '\0';
		}
		vc_echo_on();
		printf("\n");

		if (password[0] == '\0') {
			fprintf(stderr, "vc repos: no password provided.\n");
			return 1;
		}

		VcSession s;
		if (!vct_connect(&s, reposHost, reposPort, reposUser, password)) {
			memset(password, 0, sizeof(password));
			return 1;
		}
		memset(password, 0, sizeof(password));

		printf("\nRepositories on %s visible to %s:\n\n", reposHost, reposUser);
		int count = 0;

		void list_repo_cb(const char *path, const char *tag, void *ud) {
			int *n = (int *)ud;
			printf("  %-40s  %s\n", path, tag);
			(*n)++;
		}
		vct_list_repos(&s, list_repo_cb, &count);
		vct_disconnect(&s);

		if (count == 0)
			printf("  (no repositories found)\n");
		else
			printf("\n%d repo%s found.\n", count, count == 1 ? "" : "s");
		return 0;
		return vcList(argc, argv);
	} else if (strcasecmp(command, "init") == 0) {
		if (access(".vc", F_OK) == 0) {
			printf("This directory is already controlled by vc.\n");
            exit(1);
		}
		return vcInit(argc, argv);
	} else if (strcasecmp(command, "version") == 0) {
		printVersion();
	} else if (strcasecmp(command, "status") == 0) {
		bool remoteCheck = false;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--remote") == 0) remoteCheck = true;
		}
		return vcStatus(remoteCheck);
	} else if (strcasecmp(command, "push") == 0) {
		return vcPush(argc, argv);
	} else if (strcasecmp(command, "pull") == 0) {
		return vcPull(argc, argv);
	} else if (strcasecmp(command, "config") == 0) {
		if (argc >= 3 && strcmp(argv[2], "--show") == 0)
			return vcConfigShow(config);
		else if (argc >= 5 && strcmp(argv[2], "--set") == 0)
			return vcConfigSet(config, argv[3], argv[4]);
		else if (argc >= 4 && strcmp(argv[2], "--set") == 0) {
			printErr("Usage: vc config --set <key> <value>\n");
			return -1;
		} else
			return vcConfig(config);
	} else if (strcasecmp(command, "clone") == 0) {
		if (argc < 3) {
			printErr("Usage: vc clone user@host:/repo/path\n");
			help(prgName, -1);
		}
		return vcClone(argc, argv);
	} else if (strcasecmp(command, "add") == 0) {
		return vcAdd(argc, argv);
	} else if (strcasecmp(command, "archive") == 0) {
		return vcArchive(argc, argv);
	} else if (strcasecmp(command, "branch") == 0) {
		if (argc >= 4 && strcmp(argv[2], "--delete") == 0) {
			char opt[MAX_DIR_PATH];
			snprintf(opt, sizeof(opt), "--delete=%s", argv[3]);
			return vcBranch(opt);
		} else {
			return vcBranch(argc >= 3 ? argv[2] : NULL);
		}
	} else if (strcasecmp(command, "commit") == 0) {
		return vcCommit(argc, argv);
	} else if (strcasecmp(command, "tag") == 0) {
		return vcTag(argc, argv);
	} else if (strcasecmp(command, "delete") == 0) {
		return vcDelete(argc, argv);
	} else if (strcasecmp(command, "diff") == 0) {
		if (argc < 3) {
			printErr("No argument for diff command.\n");
			help(prgName, -1);
		}
		return vcDiff(argc, argv);
	} else if (strcasecmp(command, "rename") == 0) {
		if (argc < 4) {
			printErr("No oldName and/or newName for rename command.\n");
			help(prgName, -1);
		}
		return vcRename(argv[2], argv[3]);
	} else if (strcasecmp(command, "revert") == 0) {
		if (argc < 3) {
			printErr("No argument for revert command.\n");
			help(prgName, -1);
		}
		return vcRevert(argv[2]);
	} else if (strcasecmp(command, "checkout") == 0) {
		return vcCheckout(argc, argv);
	} else {
		printErr("Option unknown '%s'\n", command);
		help(prgName, -1);
	}

	return 0;
}

char *findProjectDir() {
	static char gitDirPath[MAX_DIR_PATH];
	char *workingDir;

	workingDir = getcwd(NULL, 0);

	if (workingDir == NULL) {
		// cwd has been deleted (e.g. we were inside a branch-specific
		// directory that was removed during a branch switch).
		// /proc/self/cwd still knows the old path on Linux.
		static char procBuf[MAX_DIR_PATH];
		ssize_t n = readlink("/proc/self/cwd", procBuf, sizeof(procBuf)-1);
		if (n > 0) {
			procBuf[n] = '\0';
			// readlink appends " (deleted)" when the dir no longer exists.
			char *del = strstr(procBuf, " (deleted)");
			if (del != NULL)
				*del = '\0';
			workingDir = procBuf;
		} else {
			printErr("Current directory no longer exists.\n"
			         "Please cd to your project root and try again.\n");
			exit(1);
		}
	}

	for ( ;; ) {
		snprintf(gitDirPath, sizeof(gitDirPath), "%s/.vc", workingDir);

		if (access(gitDirPath, F_OK) == 0) {
			char *s = strrchr(gitDirPath, '/');
			if (s != NULL) {
				*s = '\0';
			}
			return gitDirPath;
		}

		char *p = strrchr(workingDir, '/');
		if (p != NULL) {
			*p = '\0';
		} else {
			break;
		}

		if (workingDir[0] == '\0') {
			return NULL;
		}
	}

	return NULL;
}

int getProject() {
	if (prjName == NULL) {
		prjName = (char *)malloc(MAX_PROJECT_NAME+1);
	}

	memset(prjName, 0, MAX_PROJECT_NAME+1);

	prjName = readline("Enter Project Name: ");

	return 0;
}

int getEmail() {
	if (email == NULL) {
		email = (char *)malloc(MAX_EMAIL+1);
	}

	memset(email, 0, MAX_EMAIL+1);

	email = readline("Enter Email: ");

	return 0;
}

int getRepo() {
	if (repoPath == NULL) {
		repoPath = (char *)malloc(MAX_REPO_NAME+1);
	}

	memset(repoPath, 0, MAX_REPO_NAME+1);

	repoPath = readline("Enter Repo Path: ");

	return 0;
}

int getUserName() {
	if (userName == NULL) {
		userName = (char *)malloc(MAX_USER_NAME+1);
	}
	memset(userName, 0, MAX_USER_NAME);

	userName = readline("Enter User Name: ");

	return 0;
}

int getHostName() {
	if (hostName == NULL) {
		hostName = (char *)malloc(MAX_HOST_NAME+1);
	}
	memset(hostName, 0, MAX_HOST_NAME);

	hostName = readline("Enter Host Name: ");

	return 0;
}

char *mkZipName() {
	char *name = NULL;

	time_t t = time(NULL);
    struct tm *tm = localtime(&t);

	name = (char *)calloc(1, strlen(VC_DATA_DIR) + 24);

	int n = sprintf(name, "%s", VC_DATA_DIR);
	strftime(&name[n], sizeof(name), "/%Y%m%d_%H%M%S.zip", tm);

	return name;
}

// -------------------------------------------------------------------
// trim  –  strip leading and trailing whitespace from str in-place.
// Replaces the external libstrutils trim().
// -------------------------------------------------------------------
void trim(char *str) {
    if (str == NULL) return;

    // Strip trailing whitespace.
    int len = (int)strlen(str);
    while (len > 0 && (str[len-1] == ' '  || str[len-1] == '\t' ||
                       str[len-1] == '\n' || str[len-1] == '\r'))
        str[--len] = '\0';

    // Strip leading whitespace by shifting the string left.
    int start = 0;
    while (str[start] == ' ' || str[start] == '\t') start++;
    if (start > 0) {
        int i;
        for (i = 0; str[start + i]; i++)
            str[i] = str[start + i];
        str[i] = '\0';
    }
}

// -------------------------------------------------------------------
// parse  –  split str on any character in delimiters, storing up to
// maxArgs non-empty tokens in args[].  Modifies str in-place.
// Returns the number of tokens found.
// Replaces the external libstrutils parse().
// -------------------------------------------------------------------
int parse(char *str, const char *delimiters, char **args, int maxArgs) {
    int count = 0;
    if (str == NULL || delimiters == NULL || args == NULL) return 0;

    char *p = str;
    while (*p && count < maxArgs) {
        // Skip leading delimiters.
        while (*p && strchr(delimiters, *p)) p++;
        if (*p == '\0') break;

        // Token starts here.
        args[count++] = p;

        // Advance to next delimiter.
        while (*p && !strchr(delimiters, *p)) p++;
        if (*p) {
            *p = '\0';   // null-terminate token
            p++;
        }
    }
    return count;
}

Config *loadConfig() {
	Config *cfg = (Config *)calloc(1, sizeof(Config));

	FILE *f = fopen(VC_CFGFILE, "r");
	if (f != NULL) {
		char line[256];

		while ((fgets(line, sizeof(line), f)) != NULL) {
			trim(line);

			if (line[0] == '#' || line[0] == '[' || line[0] == '\0')
				continue;

			// Split on '=' only so values with spaces are preserved.
			char *eq = strchr(line, '=');
			if (eq == NULL) continue;
			*eq = '\0';
			char *key = line;
			char *val = eq + 1;
			trim(key);
			trim(val);
			if (key[0] == '\0' || val[0] == '\0') continue;

			if (strcasecmp(key, "user") == 0) {
				strcpy(cfg->user, val);
			} else if (strcasecmp(key, "loginName") == 0) {
				strcpy(cfg->loginName, val);
				// Migrate legacy loginName to vcdUser.
				if (cfg->vcdUser[0] == '\0')
					strcpy(cfg->vcdUser, val);
			} else if (strcasecmp(key, "vcdUser") == 0) {
				strcpy(cfg->vcdUser, val);
			} else if (strcasecmp(key, "port") == 0) {
				cfg->port = atoi(val);
			} else if (strcasecmp(key, "owner") == 0) {
				strcpy(cfg->owner, val);
			} else if (strcasecmp(key, "private") == 0) {
				cfg->isPrivate = (strcasecmp(val, "true") == 0 ||
				                  strcmp(val, "1") == 0);
			} else if (strcasecmp(key, "allowedUsers") == 0) {
				strcpy(cfg->allowedUsers, val);
			} else if (strcasecmp(key, "host") == 0) {
				strcpy(cfg->host, val);
			} else if (strcasecmp(key, "email") == 0) {
				strcpy(cfg->email, val);
			} else if (strcasecmp(key, "repo") == 0) {
				strcpy(cfg->repo, val);
			} else if (strcasecmp(key, "project") == 0) {
				strcpy(cfg->project, val);
			}
		}

		fclose(f);
	}

	return cfg;
}

void loadIgnores() {

	FILE *f = fopen(VC_IGNORE, "r");

	if (f != NULL) {
		char line[256];

		int idx = 0;
		while ((fgets(line, sizeof(line), f)) != NULL) {
			trim(line);
			if (line[0] == '#' || line[0] == '\0') {
				continue;
			}

			ignores[idx].pattern = strdup(line);
			idx++;

			if (idx >= MAX_IGNORE) {
				break;
			}
		}

		fclose(f);
	}
}

void printErr(char *fmt, ...) {
	va_list valist;
    char errMsg[1024];

    va_start(valist, fmt);

	memset(errMsg, 0, sizeof(errMsg));

    int n = sprintf(errMsg, "%s - ERROR: ", prgName);
    vsprintf(&errMsg[n], fmt, valist);
    va_end(valist);

	printf("%s\n", errMsg);
}

bool isDir(char *dirPath) {
	if (access(dirPath, F_OK) == 0) {
		return true; // Directory exists
	}
	return false;
}

uid_t getOwner(char *owner) {

	struct passwd *pwd = getpwnam(owner);
	if (pwd == NULL) {
		perror("Error getting owner UID");
		return 1;
	}

	return 0;
}

gid_t getGroup(char *group) {

	struct group *grp = getgrnam(group);
	if (grp == NULL) {
		perror("Error getting group GID");
		return 1;
	}

	return 0;
}

int isValidOctet(const char *s) {
    if (s == NULL || *s == '\0') {
        return 0; // Empty or NULL string
    }

    // Check for leading zeros (unless the octet is "0")
    if (s[0] == '0' && s[1] != '\0') {
        return 0;
    }

    int num = 0;
    int len = 0;

    while (*s != '\0') {
        if (!isdigit(*s)) {
            return 0; // Non-digit character found
        }
        num = num * 10 + (*s - '0');
        len++;
        s++;
    }

    if (len == 0 || len > 3 || num < 0 || num > 255) {
        return 0; // Invalid length or out of range
    }
    return 1;
}

int isValidIpv4(const char *ip) {
    if (ip == NULL) {
        return 0;
    }

    char temp_ip[strlen(ip) + 1]; // Create a mutable copy
    strcpy(temp_ip, ip);

    char *token;
    int octet_count = 0;

    token = strtok(temp_ip, ".");

    while (token != NULL) {
        if (!isValidOctet(token)) {
            return 0; // Invalid octet
        }
        octet_count++;
        token = strtok(NULL, ".");
    }

    return (octet_count == 4); // Must have exactly 4 octets
}

void printVersion() {
	printf("vc version %s  (built %s %s)\n", APP_VERSION, VC_BUILD_DATE, VC_BUILD_TIME);
	exit(0);
}

void vcPrintBanner() {
	char *branch = vcBranchCurrentName();
	const char *project = (config && config->project[0]) ? config->project : "(unknown)";
	printf("Project : %s  |  Branch : %s\n",
	       project,
	       branch ? branch : "main");
	printf("---\n");
	free(branch);
}

void printConfig() {
	printf("  user = %s\n", config->user);
	printf("  host = %s\n", config->host);
	printf("  email = %s\n", config->email);
	printf("  project = %s\n", config->project);
	printf("  repo = %s\n", config->repo);
}

void help(char *name, int ext) {
	printf("\nUsage: %s <command> [options]\n\n", name);

	printf("Commands:\n\n");

	printf("  add     [--binary=stage|ignore] [<file|directory> ...]\n");
	printf("           Stage files for the next commit.\n");
	printf("           Binary files prompt to add to .vcignore unless:\n");
	printf("             --binary=stage   silently stage all binary files\n");
	printf("             --binary=ignore  silently ignore all binary files\n\n");
	printf("          Stage files for the next commit.\n");
	printf("          With no arguments, stages everything under the project root.\n\n");

	printf("  archive [--all] [--keep <n>] [--list] [--restore <zip>]\n");
	printf("          Move old commit archives from .vc/data/ to .vc/archive/.\n");
	printf("          Default: keep the most recent commit, archive the rest.\n");
	printf("          --all          archive every commit.\n");
	printf("          --keep <n>     keep the n most recent commits.\n");
	printf("          --list         show archived commits.\n");
	printf("          --restore <zip> move a zip back to active commits.\n\n");

	printf("  branch  [<name>] [--delete <name>]\n");
	printf("          With no argument, list all branches (* = active).\n");
	printf("          With a name, switch to that branch if it exists,\n");
	printf("          or create it and switch to it if it does not.\n");
	printf("          --delete <name> removes a branch.\n\n");

	printf("  checkout <file> | --all | --tag <name> | --zip <file>\n");
	printf("          Restore files from a commit archive.\n");
	printf("          <file>        restore one file from the last commit.\n");
	printf("          --all         restore all files from the last commit.\n");
	printf("          --tag <name>  restore entire project to a tagged commit.\n");
	printf("          --zip <file>  restore to a specific zip archive.\n\n");

	printf("  clone   <user@host:/repo/path>\n");
	printf("          Clone a remote vc repository into a new local directory.\n");
	printf("          Downloads all .vc/ metadata and extracts the latest commit.\n");
	printf("          The local directory is named after the repo path's last component.\n\n");

	printf("  commit  --msg \"message\"\n");
	printf("          Package all staged files into a timestamped zip archive.\n");
	printf("          The --msg flag supplies the commit message.\n");
	printf("          Omitting --msg prompts interactively.\n\n");

	printf("  config  [--show] [--set <key> <value>]\n");
	printf("          View or update repository configuration.\n");
	printf("          --show             display current config.\n");
	printf("          --set <key> <val>  set a field non-interactively.\n");
	printf("          Keys: user, email, project, repo, host,\n"
					"                    vcdUser, port, owner, private, allowedUsers\n\n");

	printf("  delete  [--keep|--force] <file> [<file2> ...]\n");
	printf("          Remove file(s) from tracking and delete from disk.\n");
	printf("          Prompts for confirmation before deleting.\n");
	printf("          --keep   remove from tracking only, leave file on disk.\n");
	printf("          --force  delete without prompting.\n\n");

	printf("  diff    [--diff|--meld] <file>\n");
	printf("          Show differences between working copy and last commit.\n");
	printf("          --diff  terminal diff output (default).\n");
	printf("          --meld  open in the meld GUI diff tool.\n\n");

	printf("  help    Show this help text.\n\n");

	printf("  history <file> [--count <n>]\n");
	printf("          Show all commits that included a specific file.\n\n");

	printf("  ignore  [<pattern>] [--delete <pat>] [--check <file>]\n");
	printf("          Manage .vcignore patterns.\n");
	printf("          No args: list.  <pattern>: add.  --delete: remove.\n\n");

	printf("  info    Display a repository summary.\n\n");

	printf("  init    [--user NAME] [--email EMAIL] [--project NAME]\n");
	printf("          [--repoHost HOSTNAME] [--repoLogin USERNAME] [--repoPrivate]\n");
	printf("          [--repoPath PATH]\n");
	printf("          Initialise a new repository in the current directory.\n");
	printf("          Project name defaults to the current directory name.\n");
	printf("          Remote settings (repoPath, repoHost) are optional;\n");
	printf("          set them later with: vc config --set repo <path>\n\n");

	printf("  list    [--oldest] [--count <n>]\n");
	printf("          List all commit archives with metadata.\n");
	printf("          --oldest     oldest first.  --count <n>: limit.\n\n");

	printf("  log     Display the internal operation log for this repository.\n\n");

	printf("  merge   <branch> [--ours|--theirs]\n");
	printf("          Merge another branch into the current branch.\n");
	printf("          --ours    keep current version on conflict (default).\n");
	printf("          --theirs  take incoming version on conflict.\n\n");

	printf("  move    <old> <new>\n");
	printf("          Rename a file (alias for 'vc rename').\n\n");

	printf("  pull             Pull commits from the remote repository via vcd.\n\n");

	printf("  push             Push commits to the remote repository via vcd.\n\n");

	printf("  rename  <old> <new>\n");
	printf("          Rename a tracked file or directory and update the index.\n\n");

	printf("  revert  <file>\n");
	printf("          Restore a file to its last committed state.\n\n");

	printf("  show    [<archive>] [--tag <name>]\n");
	printf("          Show full details and file list of a commit archive.\n");
	printf("          No arg: most recent commit.  --tag: tagged commit.\n\n");

	printf("  stash   [--msg \"text\"] [pop|apply|list|drop <name>]\n");
	printf("          Save staged/modified files, clean the working tree.\n");
	printf("          pop:   restore most recent stash and remove it.\n");
	printf("          apply: restore without removing.\n");
	printf("          list:  show all stashes.\n");
	printf("          drop:  delete a stash.\n\n");

	printf("  status [--remote]  Show the state of the working tree:\n");
	printf("            staged     - ready for commit (green)\n");
	printf("            modified   - changed since last add (yellow)\n");
	printf("            untracked  - not yet tracked by vc (dark grey)\n");
	printf("            missing    - tracked but deleted on disk (red)\n");
	printf("          --remote  also connect to vcd and show whether the\n");
	printf("                    remote has commits not in local (pull needed)\n");
	printf("                    or local has commits not on remote (push needed)\n\n");

	printf("  tag     [<name>] [--msg \"message\"] [--show <name>] [--delete <name>]\n");
	printf("          With no argument, list all tags.\n");
	printf("          With a name, create a tag at the current commit.\n");
	printf("          --msg supplies an optional message.\n");
	printf("          --show <name> displays tag details.\n");
	printf("          --delete <name> removes a tag.\n\n");

	printf("  auth             Show whether vcd credentials are stored locally.\n");
	printf("  auth --clear     Remove stored vcd credentials from .vc/auth.\n\n");

	printf("  moverepo <src> <dst>\n");
	printf("               Move a repo on the vcd server between users/ and shared/.\n");
	printf("               Updates local config.vc automatically.\n");
	printf("               Example: vc moverepo users/kelly/app shared/app\n");
	printf("                        vc moverepo shared/app users/kelly/app\n\n");

	printf("  repos <host> <username>\n");
	printf("               List all repositories on a vcd server visible to you.\n");
	printf("               Shows your own repos, shared repos, and other users'\n");
	printf("               public repos. Prompts for password.\n");
	printf("               Example: vc repos 192.168.0.170 kelly\n\n");

	printf("  version Print the vc version number.\n\n");

	printf("  untrack <file> [<file2> ...]\n");
	printf("          Remove file(s) from tracking, keep on disk.\n");
	printf("          (Alias for 'vc delete --keep')\n\n");

	exit(ext);
}
