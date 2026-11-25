#pragma once
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return SHA256_Init(ctx) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const void* bytes, size_t count) {
  return SHA256_Update(ctx, bytes, count) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* digest) {
  return SHA256_Final(digest, ctx) == 1 ? 0 : -1;
}
