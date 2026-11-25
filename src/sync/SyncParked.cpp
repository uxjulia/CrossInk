#include "SyncParked.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "SyncFiles.h"

namespace SyncParked {
namespace {
constexpr size_t SLOT_SIZE = 384;
constexpr uint8_t SLOT_VERSION = 1;

// Slot layout (little-endian): version, hashCount, flags, reserved, 4 x 16-byte
// hashes, percentage (float), timestamp (int64), pctQ, spine, page, pages,
// paragraph, device[32], xpath length (u16), xpath[252], CRC32 of the rest.
struct Layout {
  static constexpr size_t VERSION = 0;
  static constexpr size_t HASH_COUNT = 1;
  static constexpr size_t FLAGS = 2;
  static constexpr size_t HASHES = 4;
  static constexpr size_t PERCENTAGE = HASHES + MAX_HASHES * 16;  // 68
  static constexpr size_t TIMESTAMP = PERCENTAGE + 4;             // 72
  static constexpr size_t PCTQ = TIMESTAMP + 8;                   // 80
  static constexpr size_t SPINE = PCTQ + 4;                       // 84
  static constexpr size_t PAGE = SPINE + 2;
  static constexpr size_t PAGES = PAGE + 2;
  static constexpr size_t PARAGRAPH = PAGES + 2;
  static constexpr size_t DEVICE = PARAGRAPH + 2;              // 92
  static constexpr size_t XPATH_LENGTH = DEVICE + MAX_DEVICE;  // 124
  static constexpr size_t XPATH = XPATH_LENGTH + 2;            // 126
  static constexpr size_t CRC = SLOT_SIZE - 4;                 // 380
};
static_assert(Layout::XPATH + MAX_XPATH <= Layout::CRC, "parked slot overflow");

bool parseHex(const std::string& hex, uint8_t (&out)[16]) {
  if (hex.size() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    unsigned value = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &value) != 1) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

void encode(const Entry& entry, uint8_t (&slot)[SLOT_SIZE]) {
  memset(slot, 0, sizeof(slot));
  slot[Layout::VERSION] = SLOT_VERSION;
  slot[Layout::HASH_COUNT] = entry.hashCount;
  slot[Layout::FLAGS] = entry.hasRich ? 1 : 0;
  memcpy(slot + Layout::HASHES, entry.hashes, sizeof(entry.hashes));
  memcpy(slot + Layout::PERCENTAGE, &entry.percentage, 4);
  memcpy(slot + Layout::TIMESTAMP, &entry.timestamp, 8);
  memcpy(slot + Layout::PCTQ, &entry.pctQ, 4);
  memcpy(slot + Layout::SPINE, &entry.spine, 2);
  memcpy(slot + Layout::PAGE, &entry.page, 2);
  memcpy(slot + Layout::PAGES, &entry.pages, 2);
  memcpy(slot + Layout::PARAGRAPH, &entry.paragraph, 2);
  memcpy(slot + Layout::DEVICE, entry.device, strnlen(entry.device, MAX_DEVICE));
  const auto xpathLength = static_cast<uint16_t>(entry.xpath.size());
  memcpy(slot + Layout::XPATH_LENGTH, &xpathLength, 2);
  memcpy(slot + Layout::XPATH, entry.xpath.data(), entry.xpath.size());
  const uint32_t crc = SyncFiles::crc(slot, Layout::CRC, 0);
  memcpy(slot + Layout::CRC, &crc, 4);
}

// False for an empty, torn or unknown slot.
bool decode(const uint8_t (&slot)[SLOT_SIZE], Entry& entry) {
  uint32_t crc = 0;
  memcpy(&crc, slot + Layout::CRC, 4);
  if (slot[Layout::VERSION] != SLOT_VERSION || crc != SyncFiles::crc(slot, Layout::CRC, 0)) return false;
  entry = {};
  entry.hashCount = slot[Layout::HASH_COUNT] > MAX_HASHES ? MAX_HASHES : slot[Layout::HASH_COUNT];
  entry.hasRich = slot[Layout::FLAGS] & 1;
  memcpy(entry.hashes, slot + Layout::HASHES, sizeof(entry.hashes));
  memcpy(&entry.percentage, slot + Layout::PERCENTAGE, 4);
  memcpy(&entry.timestamp, slot + Layout::TIMESTAMP, 8);
  memcpy(&entry.pctQ, slot + Layout::PCTQ, 4);
  memcpy(&entry.spine, slot + Layout::SPINE, 2);
  memcpy(&entry.page, slot + Layout::PAGE, 2);
  memcpy(&entry.pages, slot + Layout::PAGES, 2);
  memcpy(&entry.paragraph, slot + Layout::PARAGRAPH, 2);
  memcpy(entry.device, slot + Layout::DEVICE, MAX_DEVICE);
  entry.device[MAX_DEVICE] = '\0';
  uint16_t xpathLength = 0;
  memcpy(&xpathLength, slot + Layout::XPATH_LENGTH, 2);
  if (xpathLength > MAX_XPATH) return false;
  entry.xpath.assign(reinterpret_cast<const char*>(slot + Layout::XPATH), xpathLength);
  return true;
}

// Reads every slot once; `visit(index, valid, entry)` returns false to stop.
template <typename Visit>
void forEachSlot(Visit visit) {
  FsFile file;
  if (!Storage.exists(PATH) || !Storage.openFileForRead("SYNC", PATH, file)) return;
  uint8_t slot[SLOT_SIZE];  // 384 bytes: one slot at a time keeps reads bounded
  Entry entry;
  for (uint32_t index = 0; file.read(slot, SLOT_SIZE) == static_cast<int>(SLOT_SIZE); ++index) {
    const bool valid = decode(slot, entry);
    if (!visit(index, valid, entry)) break;
  }
  file.close();
}

bool writeSlot(const uint32_t index, const uint8_t (&slot)[SLOT_SIZE]) {
  // The slot file is created on first use; an in-place write never moves the others.
  FsFile file = Storage.open(PATH, O_RDWR | O_CREAT);
  if (!file) {
    LOG_ERR("SYNC", "Cannot open parked positions");
    return false;
  }
  const bool ok = file.seekSet(static_cast<size_t>(index) * SLOT_SIZE) && file.write(slot, SLOT_SIZE) == SLOT_SIZE;
  file.close();
  if (!ok) LOG_ERR("SYNC", "Cannot write parked position slot %u", static_cast<unsigned>(index));
  return ok;
}

// Zeroes every slot whose bit is set in `mask`.
bool clearSlots(const uint32_t mask) {
  if (!mask) return true;
  uint8_t slot[SLOT_SIZE] = {};  // all zero: an empty slot
  bool ok = true;
  for (uint32_t index = 0; index < 32; ++index)
    if (mask & (1u << index)) ok &= writeSlot(index, slot);
  return ok;
}

bool sharesHash(const Entry& a, const Entry& b) {
  for (uint8_t i = 0; i < a.hashCount; ++i)
    if (b.matches(a.hashes[i])) return true;
  return false;
}
}  // namespace

bool Entry::matches(const uint8_t (&id)[16]) const {
  for (uint8_t i = 0; i < hashCount; ++i)
    if (memcmp(hashes[i], id, 16) == 0) return true;
  return false;
}

KOReaderProgress Entry::toProgress(const std::string& document) const {
  KOReaderProgress progress{};
  progress.document = document;
  progress.progress = xpath;
  progress.percentage = percentage;
  progress.device = device;
  progress.timestamp = timestamp;
  if (hasRich) {
    KOReaderRichPosition rich;
    rich.pctQ = pctQ;
    rich.spineIndex = spine;
    rich.pageNumber = page;
    rich.totalPages = pages ? pages : 1;
    if (paragraph) rich.paragraphIndex = paragraph;
    rich.xpath = xpath;
    progress.position = std::move(rich);
  }
  return progress;
}

bool park(const KOReaderProgressChange& change) {
  // Positions that can't be stored are dropped (true): only a write failure
  // is an error the caller should retry.
  if (change.progress.size() > MAX_XPATH) {
    LOG_INF("SYNC", "Not parking a position with a %u-byte xpath", static_cast<unsigned>(change.progress.size()));
    return true;
  }
  Entry entry;
  if (!parseHex(change.document, entry.hashes[0])) {
    LOG_ERR("SYNC", "Not parking a change with a malformed document id");
    return true;
  }
  entry.hashCount = 1;
  for (uint8_t i = 0; i < change.aliasCount && entry.hashCount < MAX_HASHES; ++i)
    if (parseHex(change.aliases[i], entry.hashes[entry.hashCount])) ++entry.hashCount;
  entry.percentage = change.percentage;
  entry.timestamp = change.timestamp;
  entry.xpath = change.progress;
  snprintf(entry.device, sizeof(entry.device), "%s", change.device.c_str());
  if (change.position) {
    entry.hasRich = true;
    entry.pctQ = change.position->pctQ;
    entry.spine = change.position->spineIndex;
    entry.page = change.position->pageNumber;
    entry.pages = change.position->totalPages;
    entry.paragraph = change.position->paragraphIndex.value_or(0);
  }
  // Same document: replace in place. Otherwise the first empty slot, else the
  // oldest entry (most parked books are not on this reader and never clear).
  // A merged document can share hashes with several older slots; the first is
  // replaced and the rest cleared, so no stale copy outlives it.
  int32_t target = -1;
  int32_t emptySlot = -1;
  int32_t oldest = -1;
  int64_t oldestTimestamp = INT64_MAX;
  uint32_t slots = 0;
  uint32_t duplicates = 0;  // bit per slot index (MAX_ENTRIES <= 32)
  // Compile-time bound protects the duplicate bitmask.
  // cppcheck-suppress knownConditionTrueFalse
  static_assert(MAX_ENTRIES <= 32, "duplicate mask");
  forEachSlot([&](const uint32_t index, const bool valid, const Entry& stored) {
    ++slots;
    if (!valid) {
      if (emptySlot < 0) emptySlot = static_cast<int32_t>(index);
      return true;
    }
    if (sharesHash(entry, stored)) {
      if (target < 0)
        target = static_cast<int32_t>(index);
      else if (index < 32)
        duplicates |= 1u << index;
      return true;
    }
    if (stored.timestamp < oldestTimestamp) {
      oldestTimestamp = stored.timestamp;
      oldest = static_cast<int32_t>(index);
    }
    return true;
  });
  if (target < 0) target = emptySlot;
  if (target < 0 && slots < MAX_ENTRIES) target = static_cast<int32_t>(slots);
  if (target < 0) {
    target = oldest;
    LOG_INF("SYNC", "Parked positions full; evicting the oldest");
  }
  uint8_t slot[SLOT_SIZE];
  encode(entry, slot);
  if (!writeSlot(static_cast<uint32_t>(target), slot)) return false;
  return clearSlots(duplicates);
}

bool find(const uint8_t (&id)[16], Entry& out) {
  bool found = false;
  forEachSlot([&](uint32_t, const bool valid, const Entry& stored) {
    if (!valid || !stored.matches(id)) return true;
    out = stored;
    found = true;
    return false;
  });
  return found;
}

bool find(const std::string& hexId, Entry& out) {
  uint8_t id[16];
  return parseHex(hexId, id) && find(id, out);
}

bool remove(const std::string& hexId) {
  uint8_t id[16];
  if (!parseHex(hexId, id)) return false;
  uint32_t matches = 0;
  uint32_t others = 0;
  forEachSlot([&](const uint32_t slotIndex, const bool valid, const Entry& stored) {
    if (valid && stored.matches(id) && slotIndex < 32)
      matches |= 1u << slotIndex;
    else if (valid)
      ++others;
    return true;
  });
  // The last entry gone: drop the file, so book open stops at a missing file
  // instead of reading 32 empty slots.
  if (matches && others == 0) {
    clear();
    return !Storage.exists(PATH);
  }
  return clearSlots(matches);
}

bool empty() {
  bool any = false;
  forEachSlot([&](uint32_t, const bool valid, const Entry&) {
    any = valid;
    return !any;
  });
  return !any;
}

void clear() {
  if (Storage.exists(PATH)) Storage.remove(PATH);
}
}  // namespace SyncParked
