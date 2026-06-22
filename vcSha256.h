#ifndef VC_SHA256_H
#define VC_SHA256_H

#include <stdint.h>
#include <stddef.h>

// Compute SHA-256 of data[0..len-1], write 32 raw bytes to out.
void vc_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

// Compute SHA-256 of a C string and write lowercase hex to out[65].
void vc_sha256_hex(const char *str, char out[65]);

#endif // VC_SHA256_H
