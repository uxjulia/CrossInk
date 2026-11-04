#pragma once

#include <cstdint>
#include <string>

#include "StatsUploadClient.h"

struct Clipping;

namespace ClippingsUpload {
using Cancel = bool (*)(void*);
StatsUploadClient::Result sync(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                               void* context = nullptr);
StatsUploadClient::Result download(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                                   void* context = nullptr);
StatsUploadClient::Result upload(const std::string& path, const std::string& document, Cancel cancel = nullptr,
                                 void* context = nullptr);
}  // namespace ClippingsUpload
