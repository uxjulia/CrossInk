#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

class Epub;
class GfxRenderer;
struct EpubCancellation;
struct ReaderRenderSpec;

namespace prepared_epub_layout {

inline std::string path(const std::string& cachePath) { return cachePath + "/layout/prepared.bin"; }

inline bool invalidate(const std::string& cachePath) {
  const auto receipt = path(cachePath);
  if (!Storage.exists(receipt.c_str()) || Storage.remove(receipt.c_str())) return true;
  LOG_ERR("SCT", "Could not invalidate prepared layout: %s", receipt.c_str());
  return false;
}

bool matches(const Epub& epub, GfxRenderer& renderer, const ReaderRenderSpec& spec, int spineCount,
             const EpubCancellation& cancellation);
void save(const Epub& epub, GfxRenderer& renderer, const ReaderRenderSpec& spec, int spineCount, uint32_t faces,
          const EpubCancellation& cancellation);

}  // namespace prepared_epub_layout
