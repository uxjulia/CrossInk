#include "UsbSerialFileTransfer.h"

#include <Memory.h>
#include <SyncJsonAllocator.h>

#include "UsbProgressSync.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

extern GfxRenderer renderer;

#include <AppVersion.h>
#include <Arduino.h>
#include <BookKey.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <SdCardFontSystem.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "util/BookCacheUtils.h"

#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE && !defined(SIMULATOR)
#define CROSSINK_USB_RX_OVERFLOW_ENABLED
#include <USBCDC.h>
#endif

namespace UsbSerialFileTransfer {
namespace {

constexpr uint8_t CMD_MAGIC[] = {'C', 'M', 'N', 'D'};
constexpr uint8_t ACK = 0x06;
constexpr size_t SERIAL_CHUNK_SIZE = 256;
constexpr size_t FILE_BUFFER_SIZE = 4096;
constexpr size_t PATH_BUFFER_SIZE = 256;
constexpr size_t LINE_BUFFER_SIZE = 80;
constexpr size_t REMOVE_RECURSIVE_MAX_DEPTH = 8;
constexpr uint32_t SHORT_TIMEOUT_MS = 1000;
constexpr uint32_t HEADER_TIMEOUT_MS = 2000;
constexpr uint32_t CHUNK_TIMEOUT_MS = 45000;
constexpr uint32_t WRITE_TIMEOUT_MS = 1000;
constexpr const char* TEMP_UPLOAD_PATH = "/.crosspoint/usb-upload.tmp";
constexpr const char* INTERNAL_DIR = "/.crosspoint";
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};

#ifndef CROSSINK_FIRMWARE_DEVICE_TYPE
#define CROSSINK_FIRMWARE_DEVICE_TYPE "unknown"
#endif

uint8_t commandMatchPos = 0;
char lineBuffer[LINE_BUFFER_SIZE] = {};
size_t lineBufferPos = 0;
uint8_t transferBuffer[SERIAL_CHUNK_SIZE];
// Set once per process() call from the caller's screen context; read by every command handler.
bool fileTransferAllowed = false;
std::string usbResumeBook;
uint32_t usbBookSession = 0;
bool usbSessionAtHome = false;

#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
std::atomic<uint32_t> rxDroppedBytes{0};

void onCdcEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  if (eventId != ARDUINO_USB_CDC_RX_OVERFLOW_EVENT || !eventData) return;
  const auto* const data = static_cast<const arduino_usb_cdc_event_data_t*>(eventData);
  rxDroppedBytes.fetch_add(static_cast<uint32_t>(data->rx_overflow.dropped_bytes), std::memory_order_relaxed);
}
#endif

// Only native USB exposes the CDC overflow event used to reject incomplete uploads.
#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
uint32_t rxOverflowCount() { return rxDroppedBytes.load(std::memory_order_relaxed); }

bool rxOverflowedSince(const uint32_t snapshot, uint32_t& dropped) {
  const uint32_t current = rxOverflowCount();
  if (current == snapshot) return false;
  dropped = current - snapshot;
  return true;
}

void writeRxOverflowError(const uint32_t snapshot) {
  uint32_t dropped = 0;
  (void)rxOverflowedSince(snapshot, dropped);
  logSerial.printf("ERR:rx_overflow:dropped=%lu\n", static_cast<unsigned long>(dropped));
}
#endif

bool writeRaw(const uint8_t* data, size_t length) {
  if (length == 0) return true;
  size_t sent = 0;
  const unsigned long started = millis();
  while (sent < length) {
    const size_t written = logSerial.write(data + sent, length - sent);
    serviceSerialTx();
    if (written > length - sent) break;
    sent += written;
    if (sent == length) return true;
    if (millis() - started >= WRITE_TIMEOUT_MS) break;
    vTaskDelay(1);
  }
  LOG_ERR("USB", "Serial write incomplete: %zu/%zu bytes", sent, length);
  return false;
}

bool writeLine(const char* line) { return writeRaw(reinterpret_cast<const uint8_t*>(line), strlen(line)); }

bool writeAck() { return writeRaw(&ACK, 1); }

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool writeLe32(uint32_t value) {
  uint8_t data[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
                     static_cast<uint8_t>(value >> 24)};
  return writeRaw(data, sizeof(data));
}

bool readExact(uint8_t* buffer, size_t length, uint32_t timeoutMs, size_t* receivedOut = nullptr,
               const char* busyKind = nullptr) {
  size_t received = 0;
  const unsigned long deadline = millis() + timeoutMs;
  unsigned long nextBusyAt = millis() + 3000;
  while (received < length) {
    // An ACK can still be queued when the host starts waiting for it. Keep
    // native USB moving instead of relying on the next BUSY message to flush it.
    serviceSerialTx();
    const int available = logSerial.available();
    if (available > 0) {
      const size_t wanted = std::min(length - received, static_cast<size_t>(available));
      const size_t bytesRead = logSerial.read(buffer + received, wanted);
      if (bytesRead > 0 && bytesRead <= wanted) {
        received += bytesRead;
        nextBusyAt = millis() + 3000;
        continue;
      }
    }

    if (static_cast<long>(millis() - deadline) >= 0) {
      if (receivedOut) *receivedOut = received;
      return false;
    }

    if (busyKind && static_cast<long>(millis() - nextBusyAt) >= 0) {
      logSerial.printf("BUSY:%s:%lu/%lu\n", busyKind, static_cast<unsigned long>(received),
                       static_cast<unsigned long>(length));
      nextBusyAt = millis() + 5000;
    }

    // USB transfers run on the Arduino loop task, which is intentionally not
    // subscribed to the task watchdog. Block for a tick so priority-0 idle tasks run.
    vTaskDelay(1);
  }
  if (receivedOut) *receivedOut = received;
  return true;
}

bool readPath(char* output, size_t outputSize) {
  uint8_t lenBytes[2];
  if (!readExact(lenBytes, sizeof(lenBytes), SHORT_TIMEOUT_MS)) {
    writeLine("ERR:path_len\n");
    return false;
  }

  const uint16_t pathLen = static_cast<uint16_t>(lenBytes[0] | (lenBytes[1] << 8));
  if (pathLen == 0 || pathLen >= outputSize) {
    writeLine("ERR:path_too_long\n");
    return false;
  }

  if (!readExact(reinterpret_cast<uint8_t*>(output), pathLen, HEADER_TIMEOUT_MS)) {
    writeLine("ERR:path_read\n");
    return false;
  }
  output[pathLen] = '\0';
  return true;
}

bool normalizeSerialPath(const char* inputPath, char* output, size_t outputSize) {
  if (outputSize == 0) return false;

  std::string path = inputPath && inputPath[0] ? inputPath : "/";
  if (path == "/sdcard") {
    path = "/";
  } else if (path.rfind("/sdcard/", 0) == 0) {
    path.erase(0, 7);
  }

  std::string normalized = FsHelpers::normalisePath(path);
  if (normalized.empty()) {
    normalized = "/";
  } else if (normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }

  if (normalized.size() >= outputSize) {
    writeLine("ERR:path_too_long\n");
    return false;
  }

  strncpy(output, normalized.c_str(), outputSize);
  output[outputSize - 1] = '\0';
  return true;
}

bool isProtectedPathSegment(const char* name) {
  return (!SETTINGS.showHiddenFiles && name[0] == '.') ||
         std::any_of(HIDDEN_ITEMS, HIDDEN_ITEMS + std::size(HIDDEN_ITEMS),
                     [name](const char* item) { return strcmp(name, item) == 0; });
}

bool isProtectedPath(const char* path) {
  if (!path) return true;

  int start = 0;
  const int length = strlen(path);
  while (start < length) {
    if (path[start] == '/') {
      start++;
      continue;
    }

    int end = start;
    while (end < length && path[end] != '/') {
      end++;
    }

    char segment[PATH_BUFFER_SIZE];
    const int segmentLen = end - start;
    if (segmentLen <= 0 || static_cast<size_t>(segmentLen) >= sizeof(segment)) {
      return true;
    }
    memcpy(segment, path + start, segmentLen);
    segment[segmentLen] = '\0';

    if (isProtectedPathSegment(segment)) {
      return true;
    }

    start = end + 1;
  }
  return false;
}

bool parentPath(const char* path, char* output, size_t outputSize) {
  const char* lastSlash = strrchr(path, '/');
  if (!lastSlash || lastSlash == path) {
    if (outputSize < 2) return false;
    strcpy(output, "/");
    return true;
  }

  const size_t length = lastSlash - path;
  if (length >= outputSize) return false;
  memcpy(output, path, length);
  output[length] = '\0';
  return true;
}

bool storageExistsOrRoot(const char* path) { return strcmp(path, "/") == 0 || Storage.exists(path); }

bool readNormalizedPath(char* output, size_t outputSize) {
  char rawPath[PATH_BUFFER_SIZE];
  return readPath(rawPath, sizeof(rawPath)) && normalizeSerialPath(rawPath, output, outputSize);
}

bool ensureFileTransferAllowed() {
  if (fileTransferAllowed) return true;
  writeLine("ERR:not_on_home\n");
  return false;
}

bool isDirectory(const char* path, bool& directory) {
  HalFile file = Storage.open(path);
  if (!file) return false;
  directory = file.isDirectory();
  file.close();
  return true;
}

void clearCachesForPath(const char* path) { clearBookCachePreservingUserState(path); }

bool removeRecursive(const char* path, size_t depth = 0) {
  if (isProtectedPath(path)) return false;
  if (depth > REMOVE_RECURSIVE_MAX_DEPTH) {
    LOG_ERR("USB", "Remove failed: directory nesting too deep at %s", path);
    return false;
  }

  HalFile file = Storage.open(path);
  if (!file) return false;

  if (!file.isDirectory()) {
    file.close();
    library::invalidateLibraryIndex();
    const bool removed = Storage.remove(path);
    if (removed) clearCachesForPath(path);
    return removed;
  }

  HalFile child = file.openNextFile();
  while (child) {
    char name[PATH_BUFFER_SIZE];
    child.getName(name, sizeof(name));
    child.close();

    std::string childPath(path);
    if (childPath.back() != '/') childPath += "/";
    childPath += name;

    if (!removeRecursive(childPath.c_str(), depth + 1)) {
      file.close();
      return false;
    }

    yield();
    child = file.openNextFile();
  }
  file.close();
  return Storage.rmdir(path);
}

void handleStatus() {
  char reader[32]{};
  UsbProgressSync::readerId(reader, sizeof(reader));
  char response[224];
  snprintf(response, sizeof(response),
           "STATUS:protocol=1,progress_sync=1,sync_extras=1,sync_book_scope=1,sync_current=1,sync_reader=%s,device=%s,"
           "firmware=%s,"
           "free=%u,largest=%u\n",
           reader, CROSSINK_FIRMWARE_DEVICE_TYPE, AppVersion::version(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  writeLine(response);
}

void handleList() {
  char path[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(path, sizeof(path))) return;
  if (!ensureFileTransferAllowed()) return;
  if (isProtectedPath(path)) {
    writeLine("ERR:protected_path\n");
    return;
  }

  HalFile root = Storage.open(path);
  if (!root) {
    writeLine("ERR:opendir\n");
    return;
  }
  if (!root.isDirectory()) {
    root.close();
    writeLine("ERR:not_directory\n");
    return;
  }
  root.close();

  if (!FsHelpers::directoryCanBeEnumerated(path)) {
    writeLine("ERR:list_failed\n");
    return;
  }

  root = Storage.open(path);
  if (!root) {
    writeLine("ERR:opendir\n");
    return;
  }

  logSerial.printf("DIR:%s\n", path);
  HalFile file = root.openNextFile();
  while (file) {
    char name[PATH_BUFFER_SIZE];
    file.getName(name, sizeof(name));
    if (!isProtectedPathSegment(name)) {
      if (file.isDirectory()) {
        logSerial.printf("d|%s\n", name);
      } else {
        logSerial.printf("f|%s|%lu|0\n", name, static_cast<unsigned long>(file.size()));
      }
    }
    file.close();
    yield();
    file = root.openNextFile();
  }
  if (FsHelpers::directoryIterationFailed(root)) {
    LOG_ERR("USB", "Directory listing failed before EOF: %s", path);
    root.close();
    writeLine("ERR:list_failed\n");
    return;
  }
  root.close();
  writeLine("END\n");
}

void handleMkdir() {
  char path[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(path, sizeof(path))) return;
  if (!ensureFileTransferAllowed()) return;
  if (isProtectedPath(path)) {
    writeLine("ERR:protected_path\n");
    return;
  }

  const std::string parentDirectory = FsHelpers::extractFolderPath(path);
  std::string rollbackBoundary = parentDirectory;
  while (rollbackBoundary != "/" && !Storage.exists(rollbackBoundary.c_str())) {
    rollbackBoundary = FsHelpers::extractFolderPath(rollbackBoundary);
  }

  const bool created = Storage.mkdir(path, true);
  if (created || Storage.exists(path)) {
    if (created) {
      const auto visibility = FsHelpers::directoryEntryVisibility(parentDirectory.c_str(), path);
      if (visibility != FsHelpers::DirectoryEntryVisibility::Visible) {
        if (visibility == FsHelpers::DirectoryEntryVisibility::Missing) {
          std::string rollbackPath = path;
          while (rollbackPath != rollbackBoundary) {
            Storage.rmdir(rollbackPath.c_str());
            rollbackPath = FsHelpers::extractFolderPath(rollbackPath);
          }
        }
        writeLine("ERR:mkdir_not_visible\n");
        return;
      }
      ImageFolderIndex::invalidateForPath(path);
      sdFontSystem.markRegistryDirtyForPath(path);
    }
    writeLine("OK\n");
  } else {
    writeLine("ERR:mkdir_failed\n");
  }
}

void handleWrite() {
#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
  const uint32_t rxOverflowAtStart = rxOverflowCount();
#endif
  char path[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(path, sizeof(path))) return;
#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
  if (rxOverflowCount() != rxOverflowAtStart) {
    writeRxOverflowError(rxOverflowAtStart);
    return;
  }
#endif

  uint8_t sizeBytes[4];
  if (!readExact(sizeBytes, sizeof(sizeBytes), HEADER_TIMEOUT_MS)) {
    writeLine("ERR:size\n");
    return;
  }
  const uint32_t expectedSize = readLe32(sizeBytes);

#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
  if (rxOverflowCount() != rxOverflowAtStart) {
    writeRxOverflowError(rxOverflowAtStart);
    return;
  }
#endif

  if (!ensureFileTransferAllowed()) return;
  if (strcmp(path, "/") == 0 || isProtectedPath(path)) {
    writeLine("ERR:protected_path\n");
    return;
  }

  char parent[PATH_BUFFER_SIZE];
  if (!parentPath(path, parent, sizeof(parent)) || isProtectedPath(parent)) {
    writeLine("ERR:invalid_path\n");
    return;
  }
  if (!Storage.mkdir(parent, true) && !storageExistsOrRoot(parent)) {
    writeLine("ERR:mkdir_failed\n");
    return;
  }

  bool existingIsDirectory = false;
  if (Storage.exists(path) && isDirectory(path, existingIsDirectory) && existingIsDirectory) {
    writeLine("ERR:is_directory\n");
    return;
  }

  Storage.mkdir(INTERNAL_DIR, true);
  Storage.remove(TEMP_UPLOAD_PATH);

  HalFile file;
  if (!Storage.openFileForWrite("USB", TEMP_UPLOAD_PATH, file)) {
    writeLine("ERR:fopen\n");
    return;
  }

  auto fileBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[FILE_BUFFER_SIZE]);
  if (!fileBuffer) {
    file.close();
    Storage.remove(TEMP_UPLOAD_PATH);
    writeLine("ERR:oom\n");
    return;
  }
  size_t fileBufferPos = 0;
  uint32_t bytesAccepted = 0;
  auto flushFileBuffer = [&]() {
    if (fileBufferPos == 0) return true;
    const size_t bytesToWrite = fileBufferPos;
    logSerial.printf("BUSY:write:%lu\n", static_cast<unsigned long>(bytesAccepted));
    const size_t written = file.write(fileBuffer.get(), bytesToWrite);
    fileBufferPos = 0;
    return written == bytesToWrite;
  };

  if (!writeLine("READY\n")) {
    file.close();
    Storage.remove(TEMP_UPLOAD_PATH);
    return;
  }

  uint32_t crc = 0;
  uint32_t remaining = expectedSize;
  while (remaining > 0) {
    const size_t want = std::min(static_cast<uint32_t>(sizeof(transferBuffer)), remaining);
    size_t received = 0;
    if (!readExact(transferBuffer, want, CHUNK_TIMEOUT_MS, &received, "read")) {
      file.close();
      Storage.remove(TEMP_UPLOAD_PATH);
      logSerial.printf("ERR:timeout:%lu/%lu\n", static_cast<unsigned long>(received), static_cast<unsigned long>(want));
      return;
    }

#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
    if (rxOverflowCount() != rxOverflowAtStart) {
      file.close();
      Storage.remove(TEMP_UPLOAD_PATH);
      writeRxOverflowError(rxOverflowAtStart);
      return;
    }
#endif

    crc = esp_rom_crc32_le(crc, transferBuffer, static_cast<uint32_t>(want));
    if (fileBufferPos + want > FILE_BUFFER_SIZE && !flushFileBuffer()) {
      file.close();
      Storage.remove(TEMP_UPLOAD_PATH);
      writeLine("ERR:write\n");
      return;
    }
    memcpy(fileBuffer.get() + fileBufferPos, transferBuffer, want);
    fileBufferPos += want;
    remaining -= want;
    bytesAccepted += static_cast<uint32_t>(want);
    if (fileBufferPos >= FILE_BUFFER_SIZE && !flushFileBuffer()) {
      file.close();
      Storage.remove(TEMP_UPLOAD_PATH);
      writeLine("ERR:write\n");
      return;
    }
    if (remaining > 0 && !writeAck()) {
      file.close();
      Storage.remove(TEMP_UPLOAD_PATH);
      return;
    }
    yield();
  }

  if (!flushFileBuffer()) {
    file.close();
    Storage.remove(TEMP_UPLOAD_PATH);
    writeLine("ERR:write\n");
    return;
  }
  file.close();

#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
  if (rxOverflowCount() != rxOverflowAtStart) {
    Storage.remove(TEMP_UPLOAD_PATH);
    writeRxOverflowError(rxOverflowAtStart);
    return;
  }
#endif

  if (expectedSize > 0) {
    // Tell the host the file is saved and ready for its separately written CRC.
    if (!writeAck()) {
      Storage.remove(TEMP_UPLOAD_PATH);
      return;
    }
  }

  uint8_t crcBytes[4];
  size_t crcBytesReceived = 0;
  if (!readExact(crcBytes, sizeof(crcBytes), CHUNK_TIMEOUT_MS, &crcBytesReceived, "checksum")) {
    LOG_ERR("USB", "CRC read timed out after %zu/%zu bytes", crcBytesReceived, sizeof(crcBytes));
    Storage.remove(TEMP_UPLOAD_PATH);
    writeLine("ERR:crc_missing\n");
    return;
  }

  const uint32_t expectedCrc = readLe32(crcBytes);
  if (crc != expectedCrc) {
    Storage.remove(TEMP_UPLOAD_PATH);
    logSerial.printf("ERR:crc:expected=%08lX,actual=%08lX,bytes=%lu\n", static_cast<unsigned long>(expectedCrc),
                     static_cast<unsigned long>(crc), static_cast<unsigned long>(expectedSize));
    return;
  }

  library::invalidateLibraryIndex();
  sdFontSystem.markRegistryDirtyForPath(path);
  if (Storage.exists(path)) {
    Storage.remove(path);
  }
  if (!Storage.rename(TEMP_UPLOAD_PATH, path)) {
    Storage.remove(TEMP_UPLOAD_PATH);
    writeLine("ERR:rename_failed\n");
    return;
  }

  if (!clearBookCachePreservingUserState(path, true)) {
    LOG_ERR("USB", "File saved but cache refresh failed: %s", path);
    writeLine("ERR:cache_refresh_failed\n");
    return;
  }
  ImageFolderIndex::invalidateForPath(path);
  sdFontSystem.markRegistryDirtyForPath(path);
  writeLine("OK\n");
}

void handleRemove() {
  char path[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(path, sizeof(path))) return;
  if (!ensureFileTransferAllowed()) return;
  if (strcmp(path, "/") == 0 || isProtectedPath(path)) {
    writeLine("ERR:protected_path\n");
    return;
  }
  if (!Storage.exists(path)) {
    writeLine("ERR:not_found\n");
    return;
  }

  sdFontSystem.markRegistryDirtyForPath(path);
  if (removeRecursive(path)) {
    ImageFolderIndex::invalidateForPath(path);
    sdFontSystem.markRegistryDirtyForPath(path);
    writeLine("OK\n");
  } else {
    writeLine("ERR:remove_failed\n");
  }
}

void handleRename() {
  char src[PATH_BUFFER_SIZE];
  char dst[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(src, sizeof(src))) return;
  if (!readNormalizedPath(dst, sizeof(dst))) return;
  if (!ensureFileTransferAllowed()) return;

  if (strcmp(src, "/") == 0 || strcmp(dst, "/") == 0 || isProtectedPath(src) || isProtectedPath(dst)) {
    writeLine("ERR:protected_path\n");
    return;
  }
  if (!Storage.exists(src)) {
    writeLine("ERR:not_found\n");
    return;
  }
  if (Storage.exists(dst)) {
    writeLine("ERR:target_exists\n");
    return;
  }

  char parent[PATH_BUFFER_SIZE];
  if (!parentPath(dst, parent, sizeof(parent)) || isProtectedPath(parent) || !storageExistsOrRoot(parent)) {
    writeLine("ERR:invalid_path\n");
    return;
  }

  if (bookkey::moveFile(src, dst)) {
    library::invalidateLibraryIndex();
    clearCachesForPath(src);
    if (!clearBookCachePreservingUserState(dst, true)) {
      LOG_ERR("USB", "File moved but cache refresh failed: %s", dst);
      writeLine("ERR:cache_refresh_failed\n");
      return;
    }
    ImageFolderIndex::invalidateForPath(src);
    sdFontSystem.markRegistryDirtyForPath(src);
    ImageFolderIndex::invalidateForPath(dst);
    sdFontSystem.markRegistryDirtyForPath(dst);
    writeLine("OK\n");
  } else {
    writeLine("ERR:rename_failed\n");
  }
}

void handleRead() {
  char path[PATH_BUFFER_SIZE];
  if (!readNormalizedPath(path, sizeof(path))) return;
  if (!ensureFileTransferAllowed()) return;
  if (strcmp(path, "/") == 0 || isProtectedPath(path)) {
    writeLine("ERR:protected_path\n");
    return;
  }

  bool directory = false;
  if (isDirectory(path, directory) && directory) {
    writeLine("ERR:is_directory\n");
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("USB", path, file)) {
    writeLine("ERR:fopen\n");
    return;
  }

  const uint64_t fileSize64 = file.fileSize64();
  if (fileSize64 > UINT32_MAX) {
    file.close();
    writeLine("ERR:too_large\n");
    return;
  }

  if (!writeLine("READY\n") || !writeLe32(static_cast<uint32_t>(fileSize64))) {
    file.close();
    return;
  }

  uint32_t crc = 0;
  while (file.available() > 0) {
    const int read = file.read(transferBuffer, sizeof(transferBuffer));
    if (read < 0) {
      file.close();
      writeLine("ERR:read\n");
      return;
    }
    if (read == 0) break;

    if (!writeRaw(transferBuffer, static_cast<size_t>(read))) {
      file.close();
      return;
    }
    crc = esp_rom_crc32_le(crc, transferBuffer, static_cast<uint32_t>(read));
    yield();
  }
  file.close();
  writeLe32(crc);
}

// JSON is framed and checksummed; chunk ACKs bound CDC receive-buffer use.
// The reader checkpoints through the same save path as its pulldown drawer.
// Closing the activity releases its SD handles before USB sync opens the EPUB.
void dispatchProgressSync(JsonVariantConst request, JsonDocument& response) {
  const char* operation = request["op"] | "";
  const bool starting = strcmp(operation, "begin_current") == 0;
  const bool gettingPath = strcmp(operation, "current_path") == 0;
  const bool resuming = strcmp(operation, "resume_current") == 0;
  if (starting || gettingPath || resuming) {
    char reader[32];
    if (!UsbProgressSync::readerId(reader, sizeof(reader)) || strcmp(reader, request["device_id"] | "") != 0) {
      response["error"] = "reader_changed";
      return;
    }
  }
  if (starting) {
    if (!activityManager.canSyncOpenEpubOverUsb()) {
      response["error"] = "no_open_book";
      return;
    }
    std::string path;
    if (!activityManager.closeOpenEpubForUsbSync(path)) {
      response["error"] = "checkpoint_failed";
      return;
    }
    usbResumeBook = std::move(path);
    usbSessionAtHome = false;
    if (++usbBookSession == 0) ++usbBookSession;
    response["session"] = usbBookSession;
    return;
  }
  if (gettingPath || resuming) {
    if (usbResumeBook.empty() || !fileTransferAllowed || !request["session"].is<uint32_t>() ||
        request["session"].as<uint32_t>() != usbBookSession) {
      response["error"] = "reader_changed";
      return;
    }
    if (gettingPath && !Storage.exists(usbResumeBook.c_str()) && FsHelpers::hasEpubExtension(APP_STATE.openEpubPath) &&
        Storage.exists(APP_STATE.openEpubPath.c_str())) {
      // The reader can move a newly finished book to Read during teardown.
      usbResumeBook = APP_STATE.openEpubPath;
    }
    if (gettingPath) {
      response["path"] = usbResumeBook;
    } else {
      if (!Storage.exists(usbResumeBook.c_str())) {
        response["error"] = "invalid_book";
        return;
      }
      activityManager.goToReader(std::move(usbResumeBook));
      usbResumeBook.clear();
      response["resumed"] = true;
    }
    return;
  }
  if (!fileTransferAllowed) {
    response["error"] = "sync_requires_home";
    return;
  }
  RenderLock lock;
  UsbProgressSync::handle(request, response, renderer);
}

void handleProgressSync() {
  uint8_t lengthBytes[2];
  if (!readExact(lengthBytes, sizeof(lengthBytes), SHORT_TIMEOUT_MS)) {
    writeLine("ERR:sync_header\n");
    return;
  }
  const size_t length = lengthBytes[0] | (lengthBytes[1] << 8);
  if (!activityManager.canSyncOpenEpubOverUsb() && !ensureFileTransferAllowed()) return;
  if (!length || length > UsbProgressSync::MAX_REQUEST) {
    writeLine("ERR:sync_size\n");
    return;
  }
  // Bounded, command-lifetime payloads belong on heap rather than the loop stack.
  auto input = makeUniqueNoThrow<uint8_t[]>(length + 1);
  if (!input) {
    LOG_ERR("USB", "Sync input allocation failed");
    writeLine("ERR:low_memory\n");
    return;
  }
  if (!writeLine("READY\n")) return;
  for (size_t at = 0; at < length;) {
    const size_t chunk = std::min(SERIAL_CHUNK_SIZE, length - at);
    if (!readExact(input.get() + at, chunk, CHUNK_TIMEOUT_MS) || !writeAck()) return;
    at += chunk;
  }
  uint8_t checksum[4];
  if (!readExact(checksum, sizeof(checksum), SHORT_TIMEOUT_MS) ||
      readLe32(checksum) != esp_rom_crc32_le(0, input.get(), length)) {
    writeLine("ERR:sync_crc\n");
    return;
  }
  input[length] = 0;
  SyncJsonAllocator allocator;
  JsonDocument request(&allocator);
  JsonDocument response(&allocator);
  if (deserializeJson(request, input.get(), length) || !request.is<JsonObject>()) {
    writeLine("ERR:sync_json\n");
    return;
  }
  dispatchProgressSync(request.as<JsonVariantConst>(), response);
  const size_t size = measureJson(response);
  if (response.overflowed() || !size || size > UsbProgressSync::MAX_RESPONSE) {
    writeLine("ERR:sync_response_size\n");
    return;
  }
  auto output = makeUniqueNoThrow<uint8_t[]>(size + 1);
  if (!output) {
    LOG_ERR("USB", "Sync output allocation failed");
    writeLine("ERR:low_memory\n");
    return;
  }
  serializeJson(response, output.get(), size + 1);
  char header[24];
  snprintf(header, sizeof(header), "SYNC:%u\n", static_cast<unsigned>(size));
  if (!writeLine(header) || !writeRaw(output.get(), size)) return;
  writeLe32(esp_rom_crc32_le(0, output.get(), size));
}

void handleCommand() {
  uint8_t subCommand = 0;
  if (!readExact(&subCommand, 1, SHORT_TIMEOUT_MS)) {
    writeLine("ERR:cmd\n");
    return;
  }

  if (subCommand == 'S') {
    if (activityManager.canSyncOpenEpubOverUsb() || ensureFileTransferAllowed()) handleStatus();
    return;
  }

  switch (subCommand) {
    case 'A':
      handleList();
      break;
    case 'K':
      handleMkdir();
      break;
    case 'W':
      handleWrite();
      break;
    case 'R':
      handleRemove();
      break;
    case 'N':
      handleRename();
      break;
    case 'T':
      handleRead();
      break;
    case 'P':
      handleProgressSync();
      break;
    default:
      writeLine("ERR:unknown_cmd\n");
      break;
  }
}

ProcessResult handleLine() {
  lineBuffer[lineBufferPos] = '\0';
  lineBufferPos = 0;

  if (strcmp(lineBuffer, "CMD:SCREENSHOT") == 0) {
    return ProcessResult::ScreenshotRequested;
  }
  return ProcessResult::None;
}

}  // namespace

int relaySyncHttp(void* context, const char* method, const char* endpoint, const char* payload, const char* inputPath,
                  char* response, const size_t capacity, const char* outputPath) {
  auto& failed = *static_cast<bool*>(context);
  if (failed) return 0;
  const auto exchange = [&]() -> int {
    constexpr size_t MAX_HTTP_BYTES = 32768;
    if (!capacity || capacity > MAX_HTTP_BYTES || (response && outputPath)) return 0;
    FsFile source;
    ScopedCleanup closeSource{[&] { source.close(); }};
    if (inputPath && !Storage.openFileForRead("USBSync", inputPath, source)) return 0;
    const size_t length = inputPath ? source.size() : payload ? strlen(payload) : 0;
    if (length > MAX_HTTP_BYTES) return 0;
    SyncJsonAllocator allocator;
    JsonDocument descriptor(&allocator);
    descriptor["method"] = method;
    descriptor["endpoint"] = endpoint;
    descriptor["bytes"] = length;
    descriptor["max_response"] = response ? capacity - 1 : capacity;
    const size_t metaSize = measureJson(descriptor);
    // Reuse the existing foreground serial chunk buffer for metadata and file IO.
    if (descriptor.overflowed() || metaSize >= sizeof(transferBuffer)) return 0;
    serializeJson(descriptor, transferBuffer, sizeof(transferBuffer));
    char header[24];
    snprintf(header, sizeof(header), "HTTP:%u\n", static_cast<unsigned>(metaSize));
    if (!writeLine(header) || !writeRaw(transferBuffer, metaSize)) return 0;
    uint32_t crc = esp_rom_crc32_le(0, transferBuffer, metaSize);
    uint8_t ack;
    if (!readExact(&ack, 1, SHORT_TIMEOUT_MS) || ack != ACK) return 0;
    descriptor.clear();
    for (size_t at = 0; at < length;) {
      const size_t chunk = std::min(sizeof(transferBuffer), length - at);
      if (inputPath) {
        if (source.read(transferBuffer, chunk) != static_cast<int>(chunk)) return 0;
      } else {
        memcpy(transferBuffer, payload + at, chunk);
      }
      crc = esp_rom_crc32_le(crc, transferBuffer, chunk);
      if (!writeRaw(transferBuffer, chunk) || !readExact(&ack, 1, SHORT_TIMEOUT_MS) || ack != ACK) return 0;
      at += chunk;
    }
    if (inputPath && !source.close()) return 0;
    if (!writeLe32(crc)) return 0;
    // Response header: HTTP status (u16 LE), bytes (u32 LE). The CRC covers
    // both header and body so a damaged status cannot acknowledge an upload.
    uint8_t reply[6];
    if (!readExact(reply, sizeof(reply), CHUNK_TIMEOUT_MS)) return 0;
    const unsigned status = reply[0] | (reply[1] << 8);
    const size_t size = readLe32(reply + 2);
    if ((status != 0 && (status < 100 || status > 599)) || size > (response ? capacity - 1 : capacity)) return 0;
    crc = esp_rom_crc32_le(0, reply, sizeof(reply));
    FsFile target;
    ScopedCleanup closeTarget{[&] { target.close(); }};
    if (outputPath && status == 200 && !Storage.openFileForWrite("USBSync", outputPath, target)) return 0;
    if (!writeAck()) return 0;
    for (size_t at = 0; at < size;) {
      const size_t chunk = std::min(sizeof(transferBuffer), size - at);
      if (!readExact(transferBuffer, chunk, CHUNK_TIMEOUT_MS)) return 0;
      crc = esp_rom_crc32_le(crc, transferBuffer, chunk);
      if (response) memcpy(response + at, transferBuffer, chunk);
      if (outputPath && status == 200 && target.write(transferBuffer, chunk) != chunk) return 0;
      if (!writeAck()) return 0;
      at += chunk;
    }
    uint8_t checksum[4];
    if (!readExact(checksum, sizeof(checksum), SHORT_TIMEOUT_MS) || readLe32(checksum) != crc) return 0;
    if (response) response[size] = '\0';
    if (outputPath && status == 200 && (!target.sync() || !target.close())) return 0;
    return status;
  };
  const int status = exchange();
  if (!status) {
    LOG_ERR("USBSync", "HTTP relay interrupted");
    failed = true;
  }
  return status;
}

void registerUsbCdcOverflowHandler() {
#if defined(CROSSINK_USB_RX_OVERFLOW_ENABLED)
  logSerial.onEvent(onCdcEvent);
#endif
}

ProcessResult process(bool allowed) {
  UsbProgressSync::expireBookList(allowed);
  if (!usbResumeBook.empty()) {
    if (allowed)
      usbSessionAtHome = true;
    else if (usbSessionAtHome)
      usbResumeBook.clear();
  }
  if (!logSerial) return ProcessResult::None;
  fileTransferAllowed = allowed;

  while (logSerial.available() > 0) {
    const int byteValue = logSerial.read();
    if (byteValue < 0) break;

    const uint8_t byte = static_cast<uint8_t>(byteValue);
    if (byte == CMD_MAGIC[commandMatchPos]) {
      commandMatchPos++;
      if (commandMatchPos == sizeof(CMD_MAGIC)) {
        commandMatchPos = 0;
        lineBufferPos = 0;
        handleCommand();
        return ProcessResult::None;
      }
    } else {
      commandMatchPos = byte == CMD_MAGIC[0] ? 1 : 0;
    }

    if (byte == '\n') {
      const ProcessResult result = handleLine();
      if (result != ProcessResult::None) return result;
      continue;
    }
    if (byte == '\r') continue;

    if (lineBufferPos + 1 < sizeof(lineBuffer)) {
      lineBuffer[lineBufferPos++] = static_cast<char>(byte);
    } else {
      lineBufferPos = 0;
    }
  }

  return ProcessResult::None;
}

}  // namespace UsbSerialFileTransfer
