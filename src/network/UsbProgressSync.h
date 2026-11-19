#pragma once

#include <ArduinoJson.h>

class GfxRenderer;

// One bounded, offline request. The caller holds RenderLock and only admits
// requests from Home, after the reader has flushed and released its book.
namespace UsbProgressSync {
constexpr size_t MAX_REQUEST = 12288;
constexpr size_t MAX_RESPONSE = 12288;
constexpr size_t MAX_LOCATOR_BYTES = 4096;
bool readerId(char* out, size_t capacity);
void expireBookList(bool onHome);
void handle(JsonVariantConst request, JsonDocument& response, GfxRenderer& renderer);
}  // namespace UsbProgressSync
