// Integration shim for Android: the d2r engine was written against Apple's
// CommonCrypto, which Android does not provide. Only the SHA-256 entry points
// the engine actually calls are declared here; the implementation is our own
// (see CommonDigest.c) so no Apple headers are needed at build time.
//
// The engine uses this for one thing: naming a shader bundle after the guest's
// control bytes plus the code area. Any byte-level divergence between this and
// Apple's implementation would silently rename every shader, so the
// implementation is checked against the FIPS-180 test vectors on start-up of
// the offline packer (see tools/d2r_check_sha256.py).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t buffer[64];
    size_t buffer_length;
} CC_SHA256_CTX;

void CC_SHA256_Init(CC_SHA256_CTX *ctx);
void CC_SHA256_Update(CC_SHA256_CTX *ctx, const void *data, size_t length);
void CC_SHA256_Final(uint8_t digest[32], CC_SHA256_CTX *ctx);

#ifdef __cplusplus
}
#endif
