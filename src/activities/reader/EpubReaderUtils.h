#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <optional>
#include <string>

#include "ProgressRecord.h"
#include "util/InPlaceFileWrite.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
};

inline progress_record::Slot readProgressSlot(const char* moduleName, const std::string& path) {
  if (!Storage.exists(path.c_str())) return {};
  FsFile file;
  if (!Storage.openFileForRead(moduleName, path, file)) return {};
  uint8_t data[progress_record::RECORD_BYTES] = {};
  const size_t size = file.fileSize();
  const size_t wanted = std::min(size, sizeof(data));
  const int n = file.read(data, wanted);
  file.close();
  if (n < 0 || static_cast<size_t>(n) != wanted) return {};
  return progress_record::decode(data, size);
}

inline bool copyProgress(const progress_record::Slot& slot, Progress& progress) {
  if (slot.kind == progress_record::SlotKind::Invalid) return false;
  const auto& f = slot.fields;
  progress = {f.spineIndex,        f.pageNumber == UINT16_MAX ? 0 : f.pageNumber,
              f.pageCount,         f.hasPageCount,
              f.visibleTextOffset, f.hasVisibleTextOffset};
  return true;
}

// Read one explicit file (used when importing a legacy snapshot).
inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  return copyProgress(readProgressSlot(moduleName, path), progress);
}

inline bool loadProgressPath(const std::string& path, Progress& progress, const char* moduleName = "ERS") {
  const auto primary = readProgressSlot(moduleName, path);
  const auto backup = readProgressSlot(moduleName, path + ".bak");
  const auto newest = progress_record::newest(primary, backup);
  return newest != progress_record::NONE &&
         copyProgress(newest == progress_record::PRIMARY ? primary : backup, progress);
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  return loadProgressPath(epub.getCachePath() + "/progress.bin", progress, moduleName);
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string path = epub.getCachePath() + "/progress.bin";
  const std::string backupPath = path + ".bak";
  const auto primary = readProgressSlot("ERS", path);
  const auto backup = readProgressSlot("ERS", backupPath);
  progress_record::Fields fields{static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                                 static_cast<uint16_t>(pageCount),  true,
                                 visibleTextOffset.value_or(0),     visibleTextOffset.has_value()};
  const auto newest = progress_record::newest(primary, backup);
  if (newest != progress_record::NONE &&
      progress_record::sameFields(newest == progress_record::PRIMARY ? primary.fields : backup.fields, fields))
    return true;
  uint8_t record[progress_record::RECORD_BYTES];
  progress_record::encode(fields, progress_record::nextSeq(primary, backup), record);
  const auto& target = progress_record::target(primary, backup) == progress_record::PRIMARY ? path : backupPath;
  return writeFileInPlace("ERS", target.c_str(), record, sizeof(record));
}

}  // namespace EpubReaderUtils
