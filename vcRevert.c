
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "vc.h"

// vcRevert restores a single file to its last committed state.
// It delegates to vcCheckout which owns the zip-restoration logic.
int vcRevert(char *fileName) {

	vcLog("%s %s\n", __func__, vcTopDir);

	// Build a synthetic argv so vcCheckout can be called directly.
	char *args[] = { "vc", "checkout", fileName, NULL };
	return vcCheckout(3, args);
}
