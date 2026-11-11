#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Arduino.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <HalClock.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <cassert>
#include <cstdlib>
#include <new>

#include "HalSpiBus.h"

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

#if FREEINK_CAP_USB_MSC
class HalStorage::UsbDriveContext {
 public:
  freeink::UsbMassStorage massStorage;
};
#endif

namespace {
constexpr uint16_t kFallbackYear = 2024;
constexpr uint8_t kFallbackMonth = 1;
constexpr uint8_t kFallbackDay = 1;
constexpr uint8_t kFallbackHour = 0;
constexpr uint8_t kFallbackMinute = 0;
const uint8_t* clockUtcOffsetQ = nullptr;

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return days[month - 1];
}

bool isValidFatDateTime(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                        const uint8_t minute) {
  if (year < 1980 || year > 2107 || hour > 23 || minute > 59) return false;
  const uint8_t monthDays = daysInMonth(year, month);
  return monthDays > 0 && day >= 1 && day <= monthDays;
}

void adjustDateByDays(uint16_t& year, uint8_t& month, uint8_t& day, const int dayDelta) {
  if (dayDelta > 0) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (day < monthDays) {
      day++;
      return;
    }
    day = 1;
    if (month < 12) {
      month++;
    } else {
      month = 1;
      year++;
    }
  } else if (dayDelta < 0) {
    if (day > 1) {
      day--;
      return;
    }
    if (month > 1) {
      month--;
    } else {
      month = 12;
      year--;
    }
    day = daysInMonth(year, month);
  }
}

void setPackedFatDateTime(uint16_t* date, uint16_t* time, const uint16_t year, const uint8_t month, const uint8_t day,
                          const uint8_t hour, const uint8_t minute) {
  *date = FS_DATE(year, month, day);
  *time = FS_TIME(hour, minute, 0);
}

void storageDateTimeCallback(uint16_t* date, uint16_t* time) {
  uint16_t year = kFallbackYear;
  uint8_t month = kFallbackMonth;
  uint8_t day = kFallbackDay;
  uint8_t hour = kFallbackHour;
  uint8_t minute = kFallbackMinute;

  if (halClock.getDateTime(year, month, day, hour, minute) && isValidFatDateTime(year, month, day, hour, minute)) {
    const uint8_t configuredOffsetQ = clockUtcOffsetQ ? *clockUtcOffsetQ : 48;
    const uint8_t offsetQ = configuredOffsetQ > 104 ? 104 : configuredOffsetQ;
    const int offsetQuarterHours = static_cast<int>(offsetQ) - 48;
    int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;
    const int dayDelta = localMinutes < 0 ? -1 : (localMinutes >= 1440 ? 1 : 0);
    localMinutes = ((localMinutes % 1440) + 1440) % 1440;
    adjustDateByDays(year, month, day, dayDelta);
    hour = static_cast<uint8_t>(localMinutes / 60);
    minute = static_cast<uint8_t>(localMinutes % 60);
  }

  if (!isValidFatDateTime(year, month, day, hour, minute)) {
    year = kFallbackYear;
    month = kFallbackMonth;
    day = kFallbackDay;
    hour = kFallbackHour;
    minute = kFallbackMinute;
  }

  setPackedFatDateTime(date, time, year, month, day, hour, minute);
}
}  // namespace

HalStorage::HalStorage()
#if FREEINK_CAP_USB_MSC
    : usbDriveContext(new (std::nothrow) UsbDriveContext())
#endif
{
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

HalStorage::~HalStorage() = default;

class HalStorage::StorageLock {
 public:
  StorageLock() : spiLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }

 private:
  HalSpiBus::Lock spiLock;
};

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  HalSpiBus::Lock spiLock;
  return SDCard.begin();
}

bool HalStorage::ready() const { return SDCard.ready(); }

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  if (!usbDriveContext) {
    LOG_ERR("USB", "USB Drive context allocation failed");
    return false;
  }
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SD filesystem");
    return false;
  }
  if (!usbDriveContext->massStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#elif defined(SIMULATOR) && CROSSINK_APP_CAP_USB_DRIVE
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbDriveContext && usbDriveContext->massStorage.disconnectHost();
#else
  return false;
#endif
}

bool HalStorage::usbDriveHostSuspended() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbDriveContext && usbDriveContext->massStorage.hostSuspended();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  if (usbDriveContext) usbDriveContext->massStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  if (!usbDriveContext) return UsbDriveState::Unsupported;
  switch (usbDriveContext->massStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Accessed;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

// For the rest of the methods, we acquire the mutex to ensure thread safety

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

void HalStorage::shutdown() {
  StorageLock lock;
  SDCard.shutdown();
}

uint64_t HalStorage::totalBytes() const { return SDCard.sdTotalBytes(); }

uint64_t HalStorage::usedBytes() { HAL_STORAGE_WRAPPED_CALL(sdUsedBytes); }

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  const size_t size = file.fileSize();
  if (file.isDirectory() || size > cap || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "File too large or unreadable: %s", path.c_str());
    file.close();
    return false;
  }
  out.resize(size);
  const bool readOk = size == 0 || file.read(out.data(), size) == static_cast<int>(size);
  const bool closeOk = file.close();
  if (!readOk || !closeOk) {
    LOG_ERR(moduleName, "Could not read file: %s", path.c_str());
    out.clear();
    return false;
  }
  return true;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

void HalStorage::installDateTimeCallback(const uint8_t* utcOffsetQuarterHoursBiased) {
  if (!halClock.isAvailable()) return;
  clockUtcOffsetQ = utcOffsetQuarterHoursBiased;
  FsDateTime::setCallback(storageDateTimeCallback);
  LOG_INF("SD", "Installed RTC-backed SD timestamp callback");
}

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
};

void HalFile::ImplDeleter::operator()(Impl* const impl) const {
  if (!impl) return;
  impl->~Impl();
  std::free(impl);
}

void* HalFile::allocateImplStorage() {
  // The ESP32 build disables exceptions, so `new (std::nothrow)` can still
  // terminate through libstdc++ when the allocation cannot be satisfied.
  // malloc gives this storage wrapper the recoverable failure contract callers
  // expect while preserving FsFile's value semantics.
  return std::malloc(sizeof(Impl));
}

HalFile::HalFile() = default;

HalFile::HalFile(ImplPtr impl) : impl(std::move(impl)) {}

HalFile::~HalFile() { close(); }

HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&& other) {
  if (this == &other) return *this;
  close();
  impl = std::move(other.impl);
  allocationFailed_ = other.allocationFailed_;
  iterationFailed_ = other.iterationFailed_;
  other.allocationFailed_ = false;
  other.iterationFailed_ = false;
  return *this;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  FsFile fsFile;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    fsFile = SDCard.open(path, oflag);
  }
  if (!fsFile) {
    return HalFile();
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR("SD", "OOM: HalFile wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    HalFile failed;
    failed.allocationFailed_ = true;
    return failed;
  }
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  file.close();
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    ok = SDCard.openFileForRead(moduleName, path, fsFile);
  }
  if (!ok) {
    return false;
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile read wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    return false;
  }
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  file.close();
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  }
  if (!ok) {
    return false;
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile write wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    return false;
  }
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  if (!path || path[0] == '\0') {
    return false;
  }

  HalFile dir = open(path);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return false;
  }

  char name[128];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));

    // SdFat cannot reopen or delete this path while its directory entry is open.
    entry.close();
    if (nameLen == 0) {
      dir.close();
      return false;
    }

    std::string entryPath(path);
    if (entryPath.back() != '/') {
      entryPath += '/';
    }
    entryPath += name;

    const bool removed = isDirectory ? removeDir(entryPath.c_str()) : remove(entryPath.c_str());
    if (!removed) {
      dir.close();
      return false;
    }
  }

  dir.close();
  return rmdir(path);
}

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::creationTime() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl->file.getCreateDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  impl->file.rewindDirectory();
  allocationFailed_ = false;
  // SdFat's read-error bits are sticky for the lifetime of the handle and
  // FsFile does not expose clearError(). Reopen the directory to retry after
  // an iteration failure rather than making rewind appear to clear it.
}
bool HalFile::close() {
  if (!impl) return true;
  HalStorage::StorageLock lock;
  const bool ok = impl->file.close();
  impl.reset();
  allocationFailed_ = false;
  iterationFailed_ = false;
  return ok;
}
HalFile HalFile::openNextFile() {
  allocationFailed_ = false;
  iterationFailed_ = false;
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  auto fsFile = impl->file.openNextFile();
  if (!fsFile) {
    const uint8_t error = impl->file.getError();
    if (error != 0) {
      iterationFailed_ = true;
      LOG_ERR("SD", "Directory iteration failed (SdFat error 0x%02x)", error);
    }
    return HalFile();
  }
  void* const storage = allocateImplStorage();
  ImplPtr childImpl(storage ? ::new (storage) Impl(std::move(fsFile)) : nullptr);
  if (!childImpl) {
    allocationFailed_ = true;
    LOG_ERR("SD", "OOM: HalFile directory entry wrapper (%u free, %u max alloc)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    fsFile.close();
    return HalFile();
  }
  return HalFile(std::move(childImpl));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
