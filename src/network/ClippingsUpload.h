#pragma once

#include <cstdint>
#include <string>

#include "StatsUploadClient.h"

struct Clipping;

namespace ClippingsUpload {
using Cancel = bool (*)(void*);
StatsUploadClient::Result sync(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                               void* context = nullptr, StatsUploadClient::Relay* relay = nullptr,
                               uint32_t timeoutMs = StatsUploadClient::FOREGROUND_TIMEOUT_MS);
StatsUploadClient::Result download(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                                   void* context = nullptr, StatsUploadClient::Relay* relay = nullptr,
                                   uint32_t timeoutMs = StatsUploadClient::FOREGROUND_TIMEOUT_MS);
StatsUploadClient::Result upload(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                                 void* context = nullptr, StatsUploadClient::Relay* relay = nullptr,
                                 uint32_t timeoutMs = StatsUploadClient::FOREGROUND_TIMEOUT_MS);
}  // namespace ClippingsUpload
