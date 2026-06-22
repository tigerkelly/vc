#ifndef VC_AUTH_H
#define VC_AUTH_H

// ---------------------------------------------------------------------------
// vcAuth.h  –  secure local credential storage for vcd passwords.
//
// Passwords are encrypted with XOR against SHA-256(uid+hostname+salt)
// and stored in .vc/auth with permissions 0600 (owner-readable only).
//
// This is not cryptographically strong against a root attacker, but
// protects against casual file access — the same threat model as
// SSH private keys stored in ~/.ssh/.
// ---------------------------------------------------------------------------

#include <stdbool.h>

// Save the vcd password for the given host/user to .vc/auth.
// Returns true on success.
bool vcAuthSave(const char *vcTopDir, const char *host,
                const char *vcdUser, const char *password);

// Load the vcd password for the given host/user from .vc/auth.
// Writes the decrypted password into buf (must be >= 256 bytes).
// Returns true if found and decrypted successfully.
bool vcAuthLoad(const char *vcTopDir, const char *host,
                const char *vcdUser, char *buf, size_t bufSz);

// Remove stored credentials from .vc/auth.
void vcAuthClear(const char *vcTopDir);

// Prompt the user for their vcd password with echo disabled.
// If a stored password exists and decrypts, offers to use it.
// Always writes the password into buf.
bool vcAuthPrompt(const char *vcTopDir, const char *host,
                  const char *vcdUser, char *buf, size_t bufSz);

#endif // VC_AUTH_H
