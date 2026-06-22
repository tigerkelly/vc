
#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <crypt.h>

#include "vc.h"

char *vcSalt(int saltSize) {
	const char possibleChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:,.<>?";
    int numPossibleChars = sizeof(possibleChars) - 1;

	// vcLog("%s %s\n", __func__, vcTopDir);

	if (saltSize <= 0) {
		saltSize = SALT_SIZE;
	}

	char *salt = (char *)calloc(1, saltSize + 1);
	strcpy(salt, "$5$");

	srand(time(NULL));

	for (int i = 3; i < saltSize; i++) {
		int randomIndex = rand() % numPossibleChars;
        // Assign the character at the random index to the current position in the string
        salt[i] = possibleChars[randomIndex];
	}
	salt[saltSize] = '\0';

	return salt;
}

char *encryptString(char *passwd, char *salt) {
	static char *hashPwd = NULL;

	int len = strlen(passwd);

	if (len > MAX_PASSWD_SIZE) {
		*(passwd + MAX_PASSWD_SIZE) = '\0';
	}

	hashPwd = crypt(passwd, salt);

	return hashPwd;
}

