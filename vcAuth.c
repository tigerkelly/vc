// vcAuth.c  –  secure local credential storage for vcd passwords.

#include <stdio.h>
#include "vcPlatform.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#include "vc.h"
#include "vcAuth.h"

// ---------------------------------------------------------------------------
// SHA-256 via sha256sum command — avoids custom implementation dependency.
// Returns hex string in out (must be >= 65 bytes).
// ---------------------------------------------------------------------------
static void sha256ofString(const char *input, char *out) {
    // Write input to a temp pipe and read sha256sum output.
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "printf '%%s' '%s' | sha256sum 2>/dev/null", input);
    FILE *p = popen(cmd, "r");
    if (p) {
        if (fgets(out, 65, p) == NULL) out[0] = '\0';
        out[64] = '\0';
        pclose(p);
    } else {
        // Fallback: use a simple deterministic hash if popen fails.
        unsigned long h = 5381;
        for (const char *c = input; *c; c++)
            h = ((h << 5) + h) + (unsigned char)*c;
        snprintf(out, 65, "%016lx%016lx%016lx%016lx", h, h^0xDEADBEEF, h^0xCAFEBABE, h^0x12345678);
    }
}

#define AUTH_FILE     ".vc/auth"
#define AUTH_VERSION  "1"

// ---------------------------------------------------------------------------
// Build the encryption key from local machine identity.
// Key = SHA-256( username + ":" + hostname + ":" + uid_string + ":" + salt )
// This makes the encrypted blob machine- and user-specific.
// ---------------------------------------------------------------------------
static void buildKey(const char *salt, uint8_t key[32]) {
    char identity[512];
    char hostname[128] = "localhost";
    gethostname(hostname, sizeof(hostname));

    const char *username = getenv("USER");
    if (!username || !username[0]) username = getenv("LOGNAME");
    if (!username || !username[0]) username = "unknown";

    snprintf(identity, sizeof(identity), "%s:%s:%d:%s",
             username, hostname, (int)vc_getuid(), salt);

    char hexOut[65] = "";
    sha256ofString(identity, hexOut);

    // Convert hex string to 32 bytes for the key.
    for (int i = 0; i < 32; i++) {
        unsigned int b = 0;
        sscanf(hexOut + i*2, "%02x", &b);
        key[i] = (uint8_t)b;
    }
}

// XOR-encrypt/decrypt password using the key stream.
// Since XOR is symmetric, encrypt and decrypt are the same operation.
static void xorCrypt(const char *in, size_t inLen,
                     const uint8_t *key, char *out) {
    for (size_t i = 0; i < inLen; i++)
        out[i] = in[i] ^ key[i % 32];
    out[inLen] = '\0';
}

// Convert bytes to hex string.
static void bytesToHex(const uint8_t *bytes, size_t len, char *hex) {
    for (size_t i = 0; i < len; i++)
        snprintf(hex + i*2, 3, "%02x", bytes[i]);
}

// Convert hex string to bytes. Returns number of bytes written.
static size_t hexToBytes(const char *hex, uint8_t *bytes, size_t maxBytes) {
    size_t len = strlen(hex) / 2;
    if (len > maxBytes) len = maxBytes;
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + i*2, "%02x", &b);
        bytes[i] = (uint8_t)b;
    }
    return len;
}

// ---------------------------------------------------------------------------
// Generate a random hex salt (16 bytes = 32 hex chars).
// ---------------------------------------------------------------------------
static void genSalt(char *saltHex) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid() ^ (unsigned)vc_getuid());
    uint8_t salt[16];
    for (int i = 0; i < 16; i++)
        salt[i] = (uint8_t)(rand() & 0xff);
    bytesToHex(salt, 16, saltHex);
    saltHex[32] = '\0';
}

// ---------------------------------------------------------------------------
// Path to the auth file.
// ---------------------------------------------------------------------------
static void authPath(const char *vcTopDir, char *path, size_t sz) {
    snprintf(path, sz, "%s/" AUTH_FILE, vcTopDir);
}

// ---------------------------------------------------------------------------
// vcAuthSave
// ---------------------------------------------------------------------------
bool vcAuthSave(const char *vcTopDir, const char *host,
                const char *vcdUser, const char *password) {
    char saltHex[33];
    genSalt(saltHex);

    // Derive key from machine identity + salt.
    uint8_t key[32];
    buildKey(saltHex, key);

    // Encrypt the password.
    size_t pwLen = strlen(password);
    char encrypted[256];
    xorCrypt(password, pwLen, key, encrypted);

    // Convert encrypted bytes to hex.
    char encHex[512];
    bytesToHex((uint8_t *)encrypted, pwLen, encHex);

    // Write auth file: version:host:vcdUser:salt:encrypted_hex
    char path[MAX_DIR_PATH];
    authPath(vcTopDir, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "vcAuth: cannot write '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    fprintf(f, "%s:%s:%s:%s:%s\n",
            AUTH_VERSION, host, vcdUser, saltHex, encHex);
    fclose(f);

    // Restrict to owner-read/write only.
    vc_chmod_private(path);
    return true;
}

// ---------------------------------------------------------------------------
// vcAuthLoad
// ---------------------------------------------------------------------------
bool vcAuthLoad(const char *vcTopDir, const char *host,
                const char *vcdUser, char *buf, size_t bufSz) {
    char path[MAX_DIR_PATH];
    authPath(vcTopDir, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (f == NULL) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[--l] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        // Parse: version:host:vcdUser:salt:encrypted_hex
        char fVer[8], fHost[256], fUser[256], fSalt[64], fEnc[512];
        if (sscanf(line, "%7[^:]:%255[^:]:%255[^:]:%63[^:]:%511s",
                   fVer, fHost, fUser, fSalt, fEnc) != 5) continue;

        if (strcmp(fHost, host) != 0 || strcmp(fUser, vcdUser) != 0) continue;

        // Derive key and decrypt.
        uint8_t key[32];
        buildKey(fSalt, key);

        uint8_t encBytes[256];
        size_t encLen = hexToBytes(fEnc, encBytes, sizeof(encBytes));
        if (encLen == 0 || encLen >= bufSz) continue;

        xorCrypt((char *)encBytes, encLen, key, buf);
        buf[encLen] = '\0';
        found = true;
        break;
    }
    fclose(f);
    return found;
}

// ---------------------------------------------------------------------------
// vcAuthClear
// ---------------------------------------------------------------------------
void vcAuthClear(const char *vcTopDir) {
    char path[MAX_DIR_PATH];
    authPath(vcTopDir, path, sizeof(path));
    if (remove(path) == 0)
        printf("Stored credentials cleared.\n");
    else if (errno != ENOENT)
        fprintf(stderr, "vcAuth: cannot clear '%s': %s\n",
                path, strerror(errno));
}

// ---------------------------------------------------------------------------
// vcAuthPrompt
// ---------------------------------------------------------------------------
bool vcAuthPrompt(const char *vcTopDir, const char *host,
                  const char *vcdUser, char *buf, size_t bufSz) {
    // Try loading a stored password first.
    if (vcTopDir && vcTopDir[0]) {
        char stored[256] = "";
        if (vcAuthLoad(vcTopDir, host, vcdUser, stored, sizeof(stored))
                && stored[0] != '\0') {
            memcpy(buf, stored, strlen(stored) + 1);
            memset(stored, 0, sizeof(stored));
            return true;  // silently use stored password
        }
        memset(stored, 0, sizeof(stored));
    }

    // Not stored — prompt the user.
    printf("vcd password for %s@%s: ", vcdUser, host);
    fflush(stdout);

    vc_echo_off();

    if (fgets(buf, (int)bufSz, stdin) == NULL) {
        vc_echo_on();
        printf("\n");
        return false;
    }
    size_t l = strlen(buf);
    if (l > 0 && buf[l-1] == '\n') buf[--l] = '\0';
    if (l > 0 && buf[l-1] == '\r') buf[--l] = '\0';
    vc_echo_on();
    printf("\n");

    if (l == 0) return false;

    // Offer to save the password.
    if (vcTopDir && vcTopDir[0]) {
        printf("Save password? [y/N] ");
        fflush(stdout);
        char ans[8] = "";
        if (fgets(ans, sizeof(ans), stdin) != NULL &&
                (ans[0] == 'y' || ans[0] == 'Y')) {
            if (vcAuthSave(vcTopDir, host, vcdUser, buf))
                printf("Password saved to .vc/auth (encrypted, owner-read only).\n");
        }
    }

    return true;
}
