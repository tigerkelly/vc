#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "vc.h"

void vcWalk(char *dirPath, vcAction action) {
	DIR *dir;
    struct dirent *entry;
    struct stat st;
    char path[MAX_DIR_PATH];   // was 1024 — use MAX_DIR_PATH to avoid truncation

    if (!(dir = opendir(dirPath)))
        return;

    while ((entry = readdir(dir)) != NULL) {
        // Skip ".", ".." and ".vc"
        if (strcmp(entry->d_name, ".") == 0 ||
				strcmp(entry->d_name, "..") == 0 ||
				strcmp(entry->d_name, VC_DIR) == 0) {
            continue;
		}

        // Construct full path
        snprintf(path, sizeof(path), "%s/%s", dirPath, entry->d_name);

        if (stat(path, &st) == -1) {
            fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
            continue;
        }

		bool flag = false;
		for (int i = 0; i < MAX_IGNORE; i++) {
			if (ignores[i].pattern == NULL)
				continue;

			// Skip blank patterns — guard against vc_fnmatch("", ...).
			if (ignores[i].pattern[0] == '\0')
				continue;

			int ret = vc_fnmatch(ignores[i].pattern, entry->d_name);
			if (ret == 0) {
				flag = true;
				break;
			}
		}

		if (flag == true) {
			// Uncomment to debug ignore pattern matching:
			// fprintf(stderr, "  ignored: %s\n", entry->d_name);
			continue;
		}

		action(path);

        // If it's a directory, recurse
        if (S_ISDIR(st.st_mode)) {
            vcWalk(path, action);
        }
    }
    closedir(dir);
}
