#include "PreparedProgress.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>

#include "SyncFiles.h"

namespace PreparedProgress {
namespace {
constexpr const char* DIRECTORY = "/.crosspoint/sync-prepared";
std::string recordPath(const std::string& path) {
  char hash[16];
  snprintf(hash, sizeof(hash), "/%08lx.bin", static_cast<unsigned long>(SyncFiles::crc(path.data(), path.size(), 0)));
  return std::string(DIRECTORY) + hash;
}
bool fields(SyncFiles::Writer& file, const Saved& saved) {
  return file.pod(saved.spine) && file.pod(saved.page) && file.pod(saved.pages) && file.pod(saved.offset) &&
         file.pod(uint8_t(saved.hasOffset));
}
bool fields(SyncFiles::Reader& file, Saved& saved) {
  uint8_t flag = 0;
  if (!file.pod(saved.spine) || !file.pod(saved.page) || !file.pod(saved.pages) || !file.pod(saved.offset) ||
      !file.pod(flag) || flag > 1)
    return false;
  saved.hasOffset = flag;
  return true;
}
}  // namespace
bool begin() {
  if (Storage.exists(DIRECTORY) && !Storage.removeDir(DIRECTORY)) {
    LOG_ERR("SyncPrep", "Cannot discard previous preparation");
    return false;
  }
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) || !Storage.mkdir(DIRECTORY)) {
    LOG_ERR("SyncPrep", "Cannot create preparation directory");
    return false;
  }
  return true;
}
void clear() {
  if (Storage.exists(DIRECTORY) && !Storage.removeDir(DIRECTORY)) LOG_ERR("SyncPrep", "Cannot remove preparation");
}
bool write(const std::string& path, const Saved& saved, const KOReaderProgress& progress) {
  const auto target = recordPath(path);
  SyncFiles::Writer file(target.c_str());
  bool ok = file.open() && file.pod(uint8_t{1}) && file.string16(path) && fields(file, saved) &&
            file.string16(progress.document) && file.string16(progress.progress) && file.pod(progress.percentage) &&
            file.string16(progress.device) && file.pod(uint8_t(progress.metadata.has_value()));
  if (ok && progress.metadata) {
    const auto& m = *progress.metadata;
    ok = file.string16(m.filename) && file.string16(m.title) && file.string16(m.authors) && file.string16(m.isbn) &&
         file.string16(m.asin) && file.string16(m.series) && file.pod(uint8_t(m.seriesIndex.has_value()));
    if (ok && m.seriesIndex) ok = file.pod(*m.seriesIndex);
  }
  ok = ok && file.commit();
  if (!ok) LOG_ERR("SyncPrep", "Cannot stage %s", path.c_str());
  return ok;
}
bool read(const std::string& path, const Saved& saved, KOReaderProgress& progress) {
  const auto target = recordPath(path);
  SyncFiles::Reader file(target.c_str());
  uint8_t version = 0, metadata = 0;
  Saved original;
  std::string source;
  progress = {};
  if (file.open() == SyncFiles::Reader::Source::None || !file.pod(version) || version != 1 ||
      !file.string16(source, 1024) || source != path || !fields(file, original) || !(saved == original) ||
      !file.string16(progress.document, 32) || progress.document.size() != 32 ||
      !file.string16(progress.progress, 4096) || progress.progress.empty() || !file.pod(progress.percentage) ||
      !std::isfinite(progress.percentage) || progress.percentage < 0 || progress.percentage > 1 ||
      !file.string16(progress.device, 256) || !file.pod(metadata) || metadata > 1)
    return false;
  if (metadata) {
    auto& m = progress.metadata.emplace();
    uint8_t series = 0;
    if (!file.string16(m.filename, 1024) || !file.string16(m.title, 4096) || !file.string16(m.authors, 4096) ||
        !file.string16(m.isbn, 256) || !file.string16(m.asin, 256) || !file.string16(m.series, 4096) ||
        !file.pod(series) || series > 1)
      return false;
    if (series) {
      float value;
      if (!file.pod(value) || !std::isfinite(value)) return false;
      m.seriesIndex = value;
    }
  }
  return file.atEnd();
}
}  // namespace PreparedProgress
