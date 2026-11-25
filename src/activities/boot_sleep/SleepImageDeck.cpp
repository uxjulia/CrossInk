#include "SleepImageDeck.h"

#include <Arduino.h>
#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace SleepImageDeck {
namespace {

constexpr char MAGIC[] = {'C', 'S', 'D', 'K'};
constexpr uint8_t VERSION = 1;

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version;
  uint64_t fingerprint;
  uint16_t count;
  uint16_t lastIndex;
  uint8_t hasDisplayed;
  uint64_t checksum;
};
#pragma pack(pop)

const atomic_file::Operations FILE_OPS{
    nullptr,
    [](void*, const char* path) { return Storage.exists(path); },
    [](void*, const char* path) { return Storage.remove(path); },
    [](void*, const char* from, const char* to) { return Storage.rename(from, to); },
};

uint64_t checksum(const Header& header, const uint8_t* drawn, const size_t bytes) {
  uint64_t hash = 14695981039346656037ULL;
  const auto add = [&hash](const void* data, size_t length) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
      hash ^= p[i];
      hash *= 1099511628211ULL;
    }
  };
  add(&header, offsetof(Header, checksum));
  add(drawn, bytes);
  return hash;
}

bool hasBit(const uint8_t* bits, const uint16_t index) { return (bits[index / 8] & (1u << (index % 8))) != 0; }

struct Deck {
  Header header{};
  std::unique_ptr<uint8_t[]> drawn;
  std::string path;
  size_t bytes = 0;

  bool load(const char* indexPath, uint64_t fingerprint, uint16_t count) {
    if (count == 0) return false;
    bytes = (static_cast<size_t>(count) + 7) / 8;
    // One bit per image, at most 8192 bytes. Runtime-sized sleep scratch must
    // not use the small task stack; it is released before/after rendering.
    drawn = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (!drawn) {
      LOG_ERR("SLPDECK", "Cannot allocate %u bytes for sleep rotation", static_cast<unsigned>(bytes));
      return false;
    }
    path = std::string(indexPath) + ".deck";
    const std::string backup = path + ".bak";
    if (!atomic_file::recover(FILE_OPS, path.c_str(), backup.c_str())) {
      LOG_ERR("SLPDECK", "Cannot recover sleep rotation: %s", path.c_str());
      return false;
    }
    HalFile file = Storage.open(path.c_str());
    const bool valid = file && file.fileSize64() == sizeof(Header) + bytes &&
                       file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
                       memcmp(header.magic, MAGIC, sizeof(MAGIC)) == 0 && header.version == VERSION &&
                       header.fingerprint == fingerprint && header.count == count && header.hasDisplayed <= 1 &&
                       (header.lastIndex < count || header.lastIndex == UINT16_MAX) &&
                       file.read(drawn.get(), bytes) == static_cast<int>(bytes) &&
                       header.checksum == checksum(header, drawn.get(), bytes);
    file.close();
    if (!valid) {
      memset(drawn.get(), 0, bytes);
      memcpy(header.magic, MAGIC, sizeof(MAGIC));
      header.version = VERSION;
      header.fingerprint = fingerprint;
      header.count = count;
      header.lastIndex = UINT16_MAX;
      header.hasDisplayed = 0;
    }
    return true;
  }

  bool exhausted() const {
    for (uint32_t i = 0; i < header.count; ++i)
      if (!hasBit(drawn.get(), static_cast<uint16_t>(i))) return false;
    return true;
  }

  bool save() {
    header.checksum = checksum(header, drawn.get(), bytes);
    const std::string temporary = path + ".tmp";
    const std::string backup = path + ".bak";
    HalFile file;
    if (!Storage.openFileForWrite("SLPDECK", temporary.c_str(), file)) {
      LOG_ERR("SLPDECK", "Cannot open sleep rotation checkpoint: %s", temporary.c_str());
      return false;
    }
    const bool written =
        file.write(&header, sizeof(header)) == sizeof(header) && file.write(drawn.get(), bytes) == bytes && file.sync();
    const bool closed = file.close();
    if (!written || !closed || !atomic_file::publish(FILE_OPS, path.c_str(), temporary.c_str(), backup.c_str())) {
      LOG_ERR("SLPDECK", "Cannot save sleep rotation checkpoint: %s", path.c_str());
      Storage.remove(temporary.c_str());
      return false;
    }
    return true;
  }
};

}  // namespace

Result select(const char* indexPath, uint64_t fingerprint, uint16_t count, uint16_t& index, const uint8_t* eligible) {
  Deck deck;
  if (!deck.load(indexPath, fingerprint, count)) return Result::Error;
  const bool freshCycle = deck.exhausted();
  if (freshCycle) memset(deck.drawn.get(), 0, deck.bytes);
  uint32_t eligibleCount = 0;
  for (uint32_t i = 0; i < count; ++i) eligibleCount += !eligible || hasBit(eligible, static_cast<uint16_t>(i));
  const auto canDraw = [&](uint16_t i) {
    return !hasBit(deck.drawn.get(), i) && (!eligible || hasBit(eligible, i)) &&
           !((freshCycle || !deck.header.hasDisplayed) && eligibleCount > 1 && i == deck.header.lastIndex);
  };
  uint32_t remaining = 0;
  for (uint32_t i = 0; i < count; ++i) remaining += canDraw(static_cast<uint16_t>(i));
  // A retry must not reshuffle just because its restricted subset is exhausted.
  if (remaining == 0) return Result::NoEligible;
  uint32_t rank = static_cast<uint32_t>(random(static_cast<long>(remaining)));
  for (uint32_t i = 0; i < count; ++i) {
    if (canDraw(static_cast<uint16_t>(i)) && rank-- == 0) {
      index = static_cast<uint16_t>(i);
      return Result::Selected;
    }
  }
  return Result::NoEligible;
}

bool recordAttempt(const char* indexPath, uint64_t fingerprint, uint16_t count, uint16_t index, bool displayed) {
  Deck deck;
  if (index >= count || !deck.load(indexPath, fingerprint, count)) return false;
  if (deck.exhausted()) {
    memset(deck.drawn.get(), 0, deck.bytes);
    deck.header.hasDisplayed = 0;
  }
  deck.drawn[index / 8] |= 1u << (index % 8);
  // Failed decodes consume an attempt, but cannot replace the last image that
  // actually reached the display when avoiding repeats between rounds.
  if (displayed) {
    deck.header.lastIndex = index;
    deck.header.hasDisplayed = 1;
  }
  return deck.save();
}

bool skipInvalid(const char* indexPath, uint64_t fingerprint, uint16_t count, const uint8_t* valid) {
  Deck deck;
  if (!deck.load(indexPath, fingerprint, count)) return false;
  bool changed = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!hasBit(valid, static_cast<uint16_t>(i)) && !hasBit(deck.drawn.get(), static_cast<uint16_t>(i))) {
      deck.drawn[i / 8] |= 1u << (i % 8);
      changed = true;
    }
  }
  if (deck.exhausted()) {
    // All usable cards were displayed. Start the next round with malformed
    // cards still skipped, retaining the last successfully displayed image.
    memset(deck.drawn.get(), 0, deck.bytes);
    deck.header.hasDisplayed = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (!hasBit(valid, static_cast<uint16_t>(i))) deck.drawn[i / 8] |= 1u << (i % 8);
    }
    changed = true;
  }
  return !changed || deck.save();
}

}  // namespace SleepImageDeck
