#pragma once
#include <HalStorage.h>
#include <Print.h>

#include <cstdint>
#include <cstring>
#include <string>

// Print-based writers for the page cache serializers. They write either to a
// HalFile directly (HalFile is a Print) or through a RAM-staged
// BufferedFilePrint. Kept apart from Serialization.h so code that only needs
// HalFile helpers does not depend on Print.
namespace serialization {

template <typename T>
bool tryWritePod(Print& out, const T& value) {
  return out.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

inline bool tryWriteString(Print& out, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(out, len) && (len == 0 || out.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

// Print sink that stages small writes in a caller-owned buffer and forwards
// them to a HalFile in large chunks. Page serialization emits dozens of 1-4 byte
// fields per text line; unstaged, each one is a separate locked SD call.
//
// Bytes reach the file only on commit() (or when the buffer fills), and a write
// failure is reported by commit(), not by write(). With a null buffer or zero
// capacity it degrades to direct passthrough. Like the wrappers above, it must
// be the file's only writer until commit() returns.
class BufferedFilePrint : public Print {
 public:
  BufferedFilePrint(HalFile& file, uint8_t* buffer, const size_t capacity)
      : file(file), buf(buffer), cap(buffer ? capacity : 0) {}
  BufferedFilePrint(const BufferedFilePrint&) = delete;
  BufferedFilePrint& operator=(const BufferedFilePrint&) = delete;

  size_t write(const uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* data, const size_t len) override {
    if (!ok) return 0;
    if (fill + len > cap) {
      flushBuffer();
      if (!ok) return 0;
    }
    if (len >= cap) {  // also the cap == 0 passthrough
      const size_t written = file.write(data, len);
      ok = written == len;
      return written;
    }
    memcpy(buf + fill, data, len);
    fill += len;
    return len;
  }

  // Write any staged bytes; returns false if any write so far has failed short.
  bool commit() {
    flushBuffer();
    return ok;
  }

 private:
  void flushBuffer() {
    if (fill == 0 || !ok) return;
    ok = file.write(buf, fill) == fill;
    fill = 0;
  }

  HalFile& file;
  uint8_t* const buf;
  const size_t cap;
  size_t fill = 0;
  bool ok = true;
};

}  // namespace serialization
