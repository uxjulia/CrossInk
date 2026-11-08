#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "ProgressRecord.h"
#include "util/InPlaceFileWrite.h"

// Two-slot in-place saves for small binary files (reading stats), the policy
// progress.bin uses (ProgressRecord.h): each save overwrites the older of
// `path` and `path.bak` in place, one write instead of the temp/backup/rename
// dance, and a torn write fails its CRC so the other slot, one save older, is
// read instead.
//
// A slot record is the payload followed by u32 sequence and u32 CRC-32 of
// payload + sequence (little endian). A file of any other size is a legacy
// payload from older firmware: it is returned as-is for the caller to validate
// and replaced by the first save.
namespace two_slot {

constexpr size_t TRAILER_BYTES = 8;
// Largest slot file: global stats v3 (159 B) plus the trailer. Callers static_assert their payload fits.
constexpr size_t MAX_FILE_BYTES = 168;
constexpr size_t MAX_PAYLOAD_BYTES = MAX_FILE_BYTES - TRAILER_BYTES;

struct SlotRead {
  progress_record::Slot slot;  // kind and seq only
  size_t size = 0;             // payload bytes; for a legacy file its full size (may exceed the buffer)
};

// Reads one slot file into buf (MAX_FILE_BYTES).
inline SlotRead readSlot(const char* tag, const char* path, const size_t payloadSize, uint8_t* buf) {
  SlotRead result;
  if (payloadSize > MAX_PAYLOAD_BYTES) return result;
  // openFileForRead logs a failure for missing files; a missing slot is normal.
  if (!Storage.exists(path)) return result;
  HalFile file;
  if (!Storage.openFileForRead(tag, path, file)) return result;
  const size_t fileSize = file.fileSize();
  const size_t want = std::min(fileSize, MAX_FILE_BYTES);
  const int n = file.read(buf, want);
  file.close();
  if (n < 0 || static_cast<size_t>(n) != want) {
    LOG_ERR(tag, "Short read of %s", path);
    return result;
  }
  if (fileSize == payloadSize + TRAILER_BYTES) {
    if (progress_record::readU32(buf + payloadSize + 4) != progress_record::crc32(buf, payloadSize + 4)) {
      LOG_ERR(tag, "%s failed its CRC; using the other slot", path);
      return result;
    }
    result.slot.kind = progress_record::SlotKind::Versioned;
    result.slot.seq = progress_record::readU32(buf + payloadSize);
    result.size = payloadSize;
  } else if (fileSize > 0) {
    result.slot.kind = progress_record::SlotKind::Legacy;
    result.size = fileSize;
  }
  return result;
}

// Copies the newest slot that `accept` takes into out (MAX_PAYLOAD_BYTES) and
// returns its size, or 0 when neither slot is usable. `accept` sees the slot's
// bytes and size (a legacy size may exceed the bytes read). payloadSize is the
// current format's payload; only a file of that size plus the trailer is a slot record.
// Two bounded 168-byte buffers avoid heap churn; 336 bytes is intentional on
// the main/render tasks and independent of book/file size.
inline size_t read(const char* tag, const char* path, const char* backupPath, const size_t payloadSize,
                   bool (*accept)(const uint8_t* data, size_t size), uint8_t* out) {
  uint8_t primaryBuf[MAX_FILE_BYTES] = {};
  uint8_t backupBuf[MAX_FILE_BYTES] = {};
  SlotRead primary = readSlot(tag, path, payloadSize, primaryBuf);
  SlotRead backup = readSlot(tag, backupPath, payloadSize, backupBuf);
  if (accept && !accept(primaryBuf, primary.size)) primary.slot.kind = progress_record::SlotKind::Invalid;
  if (accept && !accept(backupBuf, backup.size)) backup.slot.kind = progress_record::SlotKind::Invalid;
  const bool backupFirst = progress_record::newest(primary.slot, backup.slot) == progress_record::BACKUP;
  for (const bool useBackup : {backupFirst, !backupFirst}) {
    const SlotRead& slot = useBackup ? backup : primary;
    const uint8_t* data = useBackup ? backupBuf : primaryBuf;
    if (slot.slot.kind == progress_record::SlotKind::Invalid || !accept(data, slot.size)) continue;
    memcpy(out, data, std::min(slot.size, MAX_PAYLOAD_BYTES));
    return slot.size;
  }
  return 0;
}

// Saves payload into the older slot in place. Returns true without writing when
// the newest slot already holds these bytes.
inline bool write(const char* tag, const char* path, const char* backupPath, const uint8_t* payload,
                  const size_t payloadSize, bool (*accept)(const uint8_t*, size_t) = nullptr) {
  if (payloadSize > MAX_PAYLOAD_BYTES) {
    LOG_ERR(tag, "Slot payload too large: %u", static_cast<unsigned>(payloadSize));
    return false;
  }
  uint8_t primaryBuf[MAX_FILE_BYTES] = {};
  uint8_t backupBuf[MAX_FILE_BYTES] = {};
  SlotRead primary = readSlot(tag, path, payloadSize, primaryBuf);
  SlotRead backup = readSlot(tag, backupPath, payloadSize, backupBuf);
  if (accept && !accept(primaryBuf, primary.size)) primary.slot.kind = progress_record::SlotKind::Invalid;
  if (accept && !accept(backupBuf, backup.size)) backup.slot.kind = progress_record::SlotKind::Invalid;
  const progress_record::SlotIndex newest = progress_record::newest(primary.slot, backup.slot);
  if (newest != progress_record::NONE) {
    const SlotRead& current = newest == progress_record::PRIMARY ? primary : backup;
    const uint8_t* currentData = newest == progress_record::PRIMARY ? primaryBuf : backupBuf;
    if (current.size == payloadSize && memcmp(currentData, payload, payloadSize) == 0) return true;
  }

  uint8_t* record = primaryBuf;  // reused: the comparison is done
  memcpy(record, payload, payloadSize);
  progress_record::writeU32(record + payloadSize, progress_record::nextSeq(primary.slot, backup.slot));
  progress_record::writeU32(record + payloadSize + 4, progress_record::crc32(record, payloadSize + 4));
  const char* target =
      progress_record::target(primary.slot, backup.slot) == progress_record::PRIMARY ? path : backupPath;
  return writeFileInPlace(tag, target, record, payloadSize + TRAILER_BYTES);
}

// Selection helper for legacy code that consumes an open file. The caller must
// use payloadLength() before interpreting the file's bytes.
inline std::string preferredPath(const char* tag, const std::string& path, size_t payloadSize,
                                 bool versionedOnly = false) {
  uint8_t buffer[MAX_FILE_BYTES] = {};
  const auto primary = readSlot(tag, path.c_str(), payloadSize, buffer);
  const auto backup = readSlot(tag, (path + ".bak").c_str(), payloadSize, buffer);
  if (versionedOnly && primary.slot.kind != progress_record::SlotKind::Versioned &&
      backup.slot.kind != progress_record::SlotKind::Versioned)
    return path;
  return progress_record::newest(primary.slot, backup.slot) == progress_record::BACKUP ? path + ".bak" : path;
}

inline size_t payloadLength(HalFile& file, size_t payloadSize) {
  const size_t size = file.fileSize();
  if (size != payloadSize + TRAILER_BYTES) return size;
  if (payloadSize > MAX_PAYLOAD_BYTES) return 0;
  uint8_t bytes[MAX_FILE_BYTES];
  const int n = file.read(bytes, size);
  const bool rewound = file.seekSet(0);
  if (n != static_cast<int>(size) || !rewound ||
      progress_record::readU32(bytes + payloadSize + 4) != progress_record::crc32(bytes, payloadSize + 4))
    return 0;
  return payloadSize;
}

}  // namespace two_slot
