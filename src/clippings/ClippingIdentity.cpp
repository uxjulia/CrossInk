#include "ClippingIdentity.h"

#include <Logging.h>

#include <cstdio>
#if defined(SIMULATOR)
#include <random>
#else
#include <esp_random.h>
#endif
#if defined(SIMULATOR) && defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(SIMULATOR)
#include <openssl/sha.h>
#else
#include <mbedtls/sha256.h>
#endif

bool clippingSyncIdentity(const uint32_t timestamp, const std::string& text, char (&id)[17]) {
  char decimal[11];
  const int length = snprintf(decimal, sizeof(decimal), "%lu", static_cast<unsigned long>(timestamp));
  uint8_t digest[32];
  // The simulator's mbedtls header is a non-cryptographic stub. Use its host's
  // real SHA256, as its MD5Builder already does for progress authentication.
#if defined(SIMULATOR) && defined(__APPLE__)
  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  CC_SHA256_Update(&context, decimal, length);
  CC_SHA256_Update(&context, text.data(), text.size());
  CC_SHA256_Final(digest, &context);
#elif defined(SIMULATOR)
  SHA256_CTX context;
  SHA256_Init(&context);
  SHA256_Update(&context, decimal, length);
  SHA256_Update(&context, text.data(), text.size());
  SHA256_Final(digest, &context);
#else
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
                  mbedtls_sha256_update(&context, reinterpret_cast<const uint8_t*>(decimal), length) == 0 &&
                  mbedtls_sha256_update(&context, reinterpret_cast<const uint8_t*>(text.data()), text.size()) == 0 &&
                  mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) {
    LOG_ERR("ClippingsSync", "Cannot hash clipping");
    return false;
  }
#endif
  for (size_t i = 0; i < 8; ++i) snprintf(id + i * 2, 3, "%02x", digest[i]);
  return true;
}

std::string newClippingSyncId() {
  uint32_t words[4];
#ifdef SIMULATOR
  std::random_device random;
  for (auto& word : words) word = random();
#else
  esp_fill_random(words, sizeof(words));
#endif
  char hex[33];
  snprintf(hex, sizeof(hex), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(words[0]),
           static_cast<unsigned long>(words[1]), static_cast<unsigned long>(words[2]),
           static_cast<unsigned long>(words[3]));
  return hex;
}
