#include "InflateReader.h"

#include <cstring>
void InflateReader::init() {
  memset(&decomp, 0, sizeof(decomp));
  uzlib_uncompress_init(&decomp, nullptr, 0);
}

bool InflateReader::initWithSegments(uint8_t* const (&segments)[RING_SEGMENTS]) {
  init();
  static_assert(RING_SEGMENTS <= UZLIB_DICT_MAX_SEGS);
  for (auto* segment : segments) {
    if (!segment) return false;
    memset(segment, 0, RING_SEGMENT_BYTES);
  }
  uzlib_uncompress_init_segmented(&decomp, segments, RING_SEGMENTS, 13);
  return true;
}

void InflateReader::setReadCallback(int (*cb)(uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (decomp.dict_size == 0) decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0 || decomp.eof) return false;
  return decomp.dest == decomp.dest_limit;
}

bool InflateReader::readExact(uint8_t* dest, size_t len) {
  if (decomp.dict_size == 0) decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len + 1;
  const int result = uzlib_uncompress(&decomp);
  return result == TINF_DONE && !decomp.eof && decomp.dest == dest + len && decomp.source == decomp.source_limit;
}
