#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

class HalFile;

enum class UsbDriveState : uint8_t {
  Unsupported,
  WaitingForHost,
  Connected,
  Accessed,
  Ejected,
  Disconnected,
  IoError,
};

class HalStorage {
 public:
  HalStorage();
  ~HalStorage();
  bool begin();
  bool ready() const;
  // Flush and stop the SD backend before deep sleep. Call only after all file
  // users have stopped; deep-sleep wake mounts the card again through begin().
  // False preserves the mount so the caller can report failure and retry.
  bool shutdown();
  bool syncFilesystem();
  uint64_t totalBytes() const;
  uint64_t usedBytes();
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out);
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);
  // Install SdFat timestamp support when an RTC-backed clock is available.
  void installDateTimeCallback(const uint8_t* utcOffsetQuarterHoursBiased);

  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  bool endUsbDrive();
  UsbDriveState usbDriveState() const;
  bool usbDriveHostSuspended() const;

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
#if FREEINK_CAP_USB_MSC
  class UsbDriveContext;
#endif
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
#if FREEINK_CAP_USB_MSC
  std::unique_ptr<UsbDriveContext> usbDriveContext;
#endif
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  struct ImplDeleter {
    void operator()(Impl* impl) const;
  };
  using ImplPtr = std::unique_ptr<Impl, ImplDeleter>;

  ImplPtr impl;
  // Invalid handles otherwise represent both ordinary EOF/open failure and a
  // wrapper-allocation failure. Registry scans need to distinguish them.
  bool allocationFailed_ = false;
  // SdFat returns an invalid child for both clean end-of-directory and a
  // failed directory read. Preserve which case ended the latest iteration.
  bool iterationFailed_ = false;

  explicit HalFile(ImplPtr impl);
  static void* allocateImplStorage();

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  // Packed FAT date/time (date << 16 | time), or 0 when unavailable.
  // Creation time orders the Library's Date Added sort; modification time
  // detects EPUB content changes for metadata cache reuse.
  uint32_t creationTime();
  uint32_t modificationTime();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  // Print's default bulk writer calls write(uint8_t) once per byte. Preserve
  // chunked ZIP/file transfers when this handle is passed through Print&.
  size_t write(const uint8_t* buf, size_t count) override { return write(static_cast<const void*>(buf), count); }
  size_t write(uint8_t b) override;
  bool sync();
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool allocationFailed() const { return allocationFailed_; }
  bool iterationFailed() const { return iterationFailed_; }
  bool isOpen() const;
  operator bool() const;
};

// Only rename FsFile to HalFile for downstream code. HalStorage.cpp includes
// SdFat's real FsFile while implementing the wrapper.
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
