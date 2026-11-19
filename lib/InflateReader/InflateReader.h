#pragma once

#include <uzlib.h>

#include <cstddef>
#include <cstdint>

// Deflate decompressor wrapping uzlib, with optional caller-owned history.
//
// Used for dictzip streaming and FontDecompressor's tiny flash-resident group
// decompressions, where uzlib's ~1KB state beats tinfl's ~11KB on the
// OOM-sensitive render path. EPUB zip entries and PNG IDAT use
// InflateStream (lib/miniz), which decodes several times faster.
class InflateReader {
 public:
  InflateReader() = default;
  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialize one-shot mode. The destination buffer holds the entire output,
  // so back-references resolve inside it without a separate 32KB dictionary.
  void init();

  // Caller-owned, four 8KB segments avoid a contiguous 32KB request for dictzip.
  // The segments must outlive all reads; this reader never owns them.
  static constexpr size_t RING_SEGMENT_BYTES = 8192;
  static constexpr size_t RING_SEGMENTS = 4;
  bool initWithSegments(uint8_t* const (&segments)[RING_SEGMENTS]);
  void setReadCallback(int (*cb)(uzlib_uncomp*));

  // Requires len + 1 bytes of destination capacity to reject extra output.
  bool readExact(uint8_t* dest, size_t len);

  // Set the entire compressed input as a contiguous memory buffer.
  // Used before the single read() call.
  void setSource(const uint8_t* src, size_t len);

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

 private:
  uzlib_uncomp decomp = {};
};
