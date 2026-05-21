#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstring>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;
constexpr char VALIDATED_PREFIX[] = {'C', 'P', 'V', '1'};
constexpr size_t VALIDATED_PREFIX_LEN = sizeof(VALIDATED_PREFIX);
constexpr size_t CHECKSUM_LEN = 4;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

// Simple lazy init — no thread-safety concern on single-core ESP32-C3.
const uint8_t* getHwKey() {
  static uint8_t key[HW_KEY_LEN] = {};
  static bool initialized = false;
  if (!initialized) {
    esp_efuse_mac_get_default(key);
    initialized = true;
  }
  return key;
}

void appendUint32LE(std::string& data, uint32_t value) {
  data.push_back(static_cast<char>(value & 0xFF));
  data.push_back(static_cast<char>((value >> 8) & 0xFF));
  data.push_back(static_cast<char>((value >> 16) & 0xFF));
  data.push_back(static_cast<char>((value >> 24) & 0xFF));
}

uint32_t readUint32LE(const std::string& data, size_t offset) {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24);
}

uint32_t updateFnv1a(uint32_t hash, const uint8_t byte) {
  hash ^= byte;
  hash *= FNV_PRIME;
  return hash;
}

uint32_t checksumForPlaintext(const std::string& plaintext) {
  uint32_t hash = FNV_OFFSET_BASIS;
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < HW_KEY_LEN; i++) {
    hash = updateFnv1a(hash, key[i]);
  }
  for (char c : plaintext) {
    hash = updateFnv1a(hash, static_cast<uint8_t>(c));
  }
  return hash;
}

bool hasValidatedPrefix(const std::string& data) {
  return data.size() >= VALIDATED_PREFIX_LEN && memcmp(data.data(), VALIDATED_PREFIX, VALIDATED_PREFIX_LEN) == 0;
}

bool decodeValidatedPayload(const std::string& payload, std::string& plaintext) {
  if (payload.size() < VALIDATED_PREFIX_LEN + CHECKSUM_LEN) {
    LOG_ERR("OBF", "Validated credential payload is too short");
    return false;
  }

  const size_t checksumOffset = VALIDATED_PREFIX_LEN;
  const uint32_t expectedChecksum = readUint32LE(payload, checksumOffset);
  plaintext = payload.substr(VALIDATED_PREFIX_LEN + CHECKSUM_LEN);
  if (checksumForPlaintext(plaintext) != expectedChecksum) {
    LOG_ERR("OBF", "Validated credential checksum mismatch");
    return false;
  }

  return true;
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  if (keyLen == 0 || key == nullptr) return;
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return "";
  std::string payload;
  payload.reserve(VALIDATED_PREFIX_LEN + CHECKSUM_LEN + plaintext.size());
  payload.append(VALIDATED_PREFIX, VALIDATED_PREFIX_LEN);
  appendUint32LE(payload, checksumForPlaintext(plaintext));
  payload += plaintext;
  xorTransform(payload);
  return base64::encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  DecodeStatus status = DecodeStatus::INVALID;
  std::string result = deobfuscateFromBase64(encoded, &status);
  if (ok) *ok = (status == DecodeStatus::VALIDATED || status == DecodeStatus::LEGACY);
  return result;
}

std::string deobfuscateFromBase64(const char* encoded, DecodeStatus* status) {
  if (status) *status = DecodeStatus::INVALID;
  if (encoded == nullptr || encoded[0] == '\0') {
    if (status) *status = DecodeStatus::EMPTY;
    return "";
  }
  size_t encodedLen = strlen(encoded);
  // First call: get required output buffer size
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    return "";
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    return "";
  }
  result.resize(decodedLen);

  xorTransform(result);
  if (hasValidatedPrefix(result)) {
    std::string plaintext;
    if (!decodeValidatedPayload(result, plaintext)) {
      return "";
    }

    if (status) *status = DecodeStatus::VALIDATED;
    return plaintext;
  }

  if (status) *status = DecodeStatus::LEGACY;
  return result;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    String encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  {
    std::string legacyPlaintext = "legacy marker collision";
    const uint8_t* key = getHwKey();
    for (size_t i = 0; i < VALIDATED_PREFIX_LEN; i++) {
      legacyPlaintext[i] = static_cast<char>(static_cast<uint8_t>(VALIDATED_PREFIX[i]) ^ key[i % HW_KEY_LEN]);
    }
    std::string legacyCiphertext = legacyPlaintext;
    xorTransform(legacyCiphertext);
    String legacyEncoded =
        base64::encode(reinterpret_cast<const uint8_t*>(legacyCiphertext.data()), legacyCiphertext.size());

    DecodeStatus status = DecodeStatus::INVALID;
    std::string decoded = deobfuscateFromBase64(legacyEncoded.c_str(), &status);
    if (status != DecodeStatus::LEGACY || decoded != legacyPlaintext) {
      LOG_ERR("OBF", "FAIL: legacy credential marker collision was not decoded as legacy");
      allPassed = false;
    }
  }
  String encoded = obfuscateToBase64("test123");
  if (encoded.length() > 0) {
    std::string tampered = encoded.c_str();
    const char replacement = tampered[tampered.size() - 1] == 'A' ? 'B' : 'A';
    tampered[tampered.size() - 1] = replacement;
    bool ok = true;
    deobfuscateFromBase64(tampered.c_str(), &ok);
    if (ok) {
      LOG_ERR("OBF", "FAIL: tampered credential validated");
      allPassed = false;
    }
  }
  // Verify obfuscated form differs from plaintext
  String enc = obfuscateToBase64("test123");
  if (enc == "test123") {
    LOG_ERR("OBF", "FAIL: obfuscated output identical to plaintext");
    allPassed = false;
  }
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
