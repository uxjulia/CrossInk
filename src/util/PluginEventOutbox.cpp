#include "PluginEventOutbox.h"

#include <DownloadFileSwap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

namespace PluginEventOutbox {
namespace {
// Three bounded path buffers (~380 bytes combined) avoid fallible string
// growth on the low-heap reader-exit path. The copy uses a 128-byte chunk.
constexpr size_t PATH_CAPACITY = DIRECTORY_CAPACITY + sizeof(NAME) - 1;
constexpr size_t SIDECAR_CAPACITY = PATH_CAPACITY + sizeof(".part") - 1;

bool copyQueue(HalFile& source, HalFile& output, size_t remaining) {
  uint8_t chunk[128];
  while (remaining > 0) {
    const size_t count = std::min(remaining, sizeof(chunk));
    if (source.read(chunk, count) != static_cast<int>(count) || output.write(chunk, count) != count) return false;
    remaining -= count;
  }
  return true;
}
}  // namespace

bool append(const char* directory, const char* line, const size_t lineSize) {
  char path[PATH_CAPACITY];
  const int pathSize = snprintf(path, sizeof(path), "%s%s", directory, NAME);
  if (pathSize < 0 || static_cast<size_t>(pathSize) >= sizeof(path) || lineSize == 0 || lineSize > MAX_EVENT_LINE ||
      line[lineSize - 1] != '\n') {
    LOG_ERR("PEVT", "Invalid outbox path or event length");
    return false;
  }
  char backup[SIDECAR_CAPACITY];
  snprintf(backup, sizeof(backup), "%s.old", path);
  if (!DownloadFileSwap::recover(path, backup)) return false;

  HalFile existing;
  size_t retained = 0;
  if (Storage.exists(path)) {
    if (!Storage.openFileForRead("PEVT", path, existing)) return false;
    const auto size = existing.fileSize();
    if (existing.isDirectory() || size > MAX_OUTBOX_BYTES + MAX_EVENT_LINE) {
      LOG_ERR("PEVT", "Invalid existing outbox: %s", path);
      existing.close();
      return false;
    }
    if (size <= MAX_OUTBOX_BYTES) {
      retained = size;
    } else if (!existing.close()) {
      LOG_ERR("PEVT", "Cannot close old outbox: %s", path);
      return false;
    }
  }

  char partial[SIDECAR_CAPACITY];
  snprintf(partial, sizeof(partial), "%s.part", path);
  HalFile output = Storage.open(partial, O_WRONLY | O_CREAT | O_TRUNC);
  const bool copied = output && copyQueue(existing, output, retained);
  const bool existingClosed = !existing || existing.close();
  const bool saved = copied && existingClosed &&
                     output.write(reinterpret_cast<const uint8_t*>(line), lineSize) == lineSize && output.sync();
  const bool outputClosed = output.close();
  if (!saved || !outputClosed || !DownloadFileSwap::publishWithBackup(path, partial, backup)) {
    LOG_ERR("PEVT", "Cannot append plugin event: %s", path);
    Storage.remove(partial);
    return false;
  }
  return true;
}
}  // namespace PluginEventOutbox
