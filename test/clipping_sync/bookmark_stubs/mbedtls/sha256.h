#pragma once
#include <cstddef>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
inline int mbedtls_sha256(const unsigned char* data, size_t size, unsigned char* digest, int) {
  return CC_SHA256(data, static_cast<CC_LONG>(size), digest) ? 0 : -1;
}
#else
#include <openssl/sha.h>
inline int mbedtls_sha256(const unsigned char* data, size_t size, unsigned char* digest, int) {
  return SHA256(data, size, digest) ? 0 : -1;
}
#endif
