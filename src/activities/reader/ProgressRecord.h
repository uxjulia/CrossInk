#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// EPUB reading-progress record and its two-slot save policy. Pure: no storage.
//
// progress.bin and progress.bin.bak are two fixed-size slots. Each save
// overwrites the older slot in place, so a save costs one data sector plus the
// directory entry instead of the old temp-file/rotate/rename sequence (about a
// dozen sector writes across the directory and both FAT copies). A torn write
// fails its CRC and the other slot, one save older, is used instead.
//
// Layout (20 bytes, little endian). Bytes 0..9 are the legacy 10-byte record,
// so firmware that predates the slots still reads its position from the first
// 10 bytes of progress.bin:
//   0  u16 spine index      6  u32 visible-text offset (0 when absent)
//   2  u16 page number     10  u8  marker 0xC5
//   4  u16 page count      11  u8  flags (bit 0: offset present)
//                          12  u32 sequence number
//                          16  u32 CRC-32 of bytes 0..15
namespace progress_record {

constexpr size_t RECORD_BYTES = 20;
constexpr uint8_t RECORD_MARKER = 0xC5;
constexpr uint8_t FLAG_HAS_VISIBLE_TEXT_OFFSET = 0x01;

struct Fields {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
};

enum class SlotKind : uint8_t { Invalid, Legacy, Versioned };

struct Slot {
  SlotKind kind = SlotKind::Invalid;
  uint32_t seq = 0;
  Fields fields;
};

enum SlotIndex : int { NONE = -1, PRIMARY = 0, BACKUP = 1 };

inline uint32_t crc32(const uint8_t* data, const size_t length) {
  // Bitwise CRC-32 (IEEE): 16 bytes per save, so a table would only cost flash.
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline void writeU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void writeU32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

inline void encode(const Fields& fields, const uint32_t seq, uint8_t out[RECORD_BYTES]) {
  memset(out, 0, RECORD_BYTES);
  writeU16(out + 0, fields.spineIndex);
  writeU16(out + 2, fields.pageNumber);
  writeU16(out + 4, fields.pageCount);
  if (fields.hasVisibleTextOffset) writeU32(out + 6, fields.visibleTextOffset);
  out[10] = RECORD_MARKER;
  out[11] = fields.hasVisibleTextOffset ? FLAG_HAS_VISIBLE_TEXT_OFFSET : 0;
  writeU32(out + 12, seq);
  writeU32(out + 16, crc32(out, 16));
}

// `data` holds the first min(fileSize, RECORD_BYTES) bytes of the file.
inline Slot decode(const uint8_t* data, const size_t fileSize) {
  Slot slot;
  if (fileSize == RECORD_BYTES) {
    if (data[10] != RECORD_MARKER || (data[11] & ~FLAG_HAS_VISIBLE_TEXT_OFFSET) != 0 ||
        readU32(data + 16) != crc32(data, 16))
      return slot;
    slot.kind = SlotKind::Versioned;
    slot.seq = readU32(data + 12);
    slot.fields.hasPageCount = true;
    slot.fields.hasVisibleTextOffset = (data[11] & FLAG_HAS_VISIBLE_TEXT_OFFSET) != 0;
  } else if (fileSize == 4 || fileSize == 6 || fileSize == 10) {
    slot.kind = SlotKind::Legacy;
    slot.fields.hasPageCount = fileSize >= 6;
    slot.fields.hasVisibleTextOffset = fileSize == 10;
  } else {
    return slot;
  }
  slot.fields.spineIndex = readU16(data + 0);
  slot.fields.pageNumber = readU16(data + 2);
  if (slot.fields.hasPageCount) slot.fields.pageCount = readU16(data + 4);
  if (slot.fields.hasVisibleTextOffset) slot.fields.visibleTextOffset = readU32(data + 6);
  return slot;
}

// Sequence numbers are compared modulo 2^32, so wrap-around never flips the order.
inline bool seqNewer(const uint32_t a, const uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

// Versioned records win over legacy data during migration. The first save
// writes the OTHER slot, preserving the sole legacy snapshot on power loss.
inline SlotIndex newest(const Slot& primary, const Slot& backup) {
  if (primary.kind == SlotKind::Versioned) {
    return backup.kind == SlotKind::Versioned && seqNewer(backup.seq, primary.seq) ? BACKUP : PRIMARY;
  }
  if (backup.kind == SlotKind::Versioned) return BACKUP;
  if (primary.kind == SlotKind::Legacy) return PRIMARY;
  return backup.kind == SlotKind::Legacy ? BACKUP : NONE;
}

inline SlotIndex target(const Slot& primary, const Slot& backup) {
  return newest(primary, backup) == PRIMARY ? BACKUP : PRIMARY;
}

inline uint32_t nextSeq(const Slot& primary, const Slot& backup) {
  bool any = false;
  uint32_t latest = 0;
  for (const Slot* slot : {&primary, &backup}) {
    if (slot->kind != SlotKind::Versioned) continue;
    if (!any || seqNewer(slot->seq, latest)) latest = slot->seq;
    any = true;
  }
  return any ? latest + 1 : 1;
}

inline bool sameFields(const Fields& a, const Fields& b) {
  return a.spineIndex == b.spineIndex && a.pageNumber == b.pageNumber && a.hasPageCount == b.hasPageCount &&
         (!a.hasPageCount || a.pageCount == b.pageCount) && a.hasVisibleTextOffset == b.hasVisibleTextOffset &&
         (!a.hasVisibleTextOffset || a.visibleTextOffset == b.visibleTextOffset);
}

}  // namespace progress_record
