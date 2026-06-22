
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "vc.h"

int _fdLog = -1;
FILE *_log = NULL;

int vcLog(char *msg, ...) {
	va_list valist;

	// vcLog("%s %s\n", __func__, vcTopDir);

	if (access(".vc/vc.log" , F_OK) != 0) {
		_fdLog = open(".vc/vc.log", 0644);
		if (_fdLog != -1) {
			write(_fdLog, "# Log file for this repo.\n", 26);
			close(_fdLog);
		}
	}

	if (_log == NULL) {
		_log = fopen(".vc/vc.log", "a+");
		if (_log == NULL) {
			return -1;
		}
	}

	va_start(valist, msg);

	struct timespec ts;
    timespec_get(&ts, TIME_UTC); // Get current time in UTC

    // Convert seconds part to struct tm (for use with strftime)
    struct tm tm_info;
    // Use gmtime_r for thread-safe POSIX alternative to gmtime, or gmtime on non-POSIX systems
    #ifdef __STDC_LIB_EXT1__
        gmtime_s(&tm_info, &ts.tv_sec);
    #else
        tm_info = *gmtime(&ts.tv_sec); // Note: gmtime is not thread-safe
    #endif

    char buffer[128];
    // Format the date and time (excluding milliseconds) using strftime
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_info);

    // Append the milliseconds part manually
    // tv_nsec is in nanoseconds, divide by 1,000,000 to get milliseconds (3 digits)
    char time_with_ms[150];
    sprintf(time_with_ms, "%s.%03ld ", buffer, ts.tv_nsec / 1000000);

	fprintf(_log, time_with_ms);
	vfprintf(_log, msg, valist);

	// Ensure the log entry ends with a newline.
	size_t msgLen = strlen(msg);
	if (msgLen == 0 || msg[msgLen - 1] != '\n') {
		fprintf(_log, "\n");
	}

	fflush(_log);

	va_end(valist);

	return 0;
}
#if(0)
int vcShowLog() {
	FILE *f = fopen(".vc/vc.log", "r");
	if (f != NULL) {
	printf("Reading file.\n");
		char line[1024];

		while (fgets(line, sizeof(line), f) != NULL) {
			printf("%s", line);
		}

		fclose(f);
	} else {
		printf("%s ERROR: %s\n", __func__, strerror(errno));
	}

	return 0;
}
#endif
