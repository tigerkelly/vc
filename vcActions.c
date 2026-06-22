#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "vc.h"

// vcDisplayAction  –  diagnostic helper, just prints the path.
int vcDisplayAction(char *path) {
    printf("%s\n", path);
    return 0;
}

// NOTE: vcAddAction    is defined in vcAdd.c    – needs access to AddCtx.
// NOTE: vcCommitAction is defined in vcCommit.c – needs access to CommitCtx.
