#include "ImageDimsProbe.h"

#include <cstdint>

namespace {
constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool isJpegStartOfFrame(const uint8_t marker) {
  return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}
}  // namespace

bool ImageDimsProbe::feed(const uint8_t byte) {
  switch (state) {
    case State::Sniff:
      if (byte == 0xFF) {
        state = State::JpegSoi;
      } else if (byte == PNG_SIGNATURE[0]) {
        state = State::PngHeader;
      } else if (byte == 'G') {
        state = State::GifHeader;
      } else {
        state = State::Failed;
        return false;
      }
      position = 1;
      return true;

    case State::PngHeader:
      // Bytes 1..7 are the signature, 12..15 are "IHDR", and 16..23
      // contain the big-endian width and height.
      if (position < 8) {
        if (byte != PNG_SIGNATURE[position]) {
          state = State::Failed;
          return false;
        }
      } else if (position >= 12 && position < 16) {
        if (byte != "IHDR"[position - 12]) {
          state = State::Failed;
          return false;
        }
      } else if (position >= 16 && position < 20) {
        width = (width << 8) | byte;
      } else if (position >= 20 && position < 24) {
        height = (height << 8) | byte;
        if (position == 23) {
          state = State::Done;
          return false;
        }
      }
      position++;
      return true;

    case State::GifHeader:
      if ((position < 4 && byte != "GIF8"[position]) || (position == 4 && byte != '7' && byte != '9') ||
          (position == 5 && byte != 'a')) {
        state = State::Failed;
        return false;
      }
      if (position == 6 || position == 7) {
        width |= static_cast<uint32_t>(byte) << ((position - 6) * 8);
      } else if (position == 8 || position == 9) {
        height |= static_cast<uint32_t>(byte) << ((position - 8) * 8);
      }
      if (position++ == 9) {
        state = State::Done;
        return false;
      }
      return true;

    case State::JpegSoi:
      if (byte != 0xD8) {
        state = State::Failed;
        return false;
      }
      state = State::JpegFf;
      return true;

    case State::JpegFf:
      if (byte != 0xFF) {
        state = State::Failed;
        return false;
      }
      state = State::JpegMarker;
      return true;

    case State::JpegMarker:
      if (byte == 0xFF) return true;
      if (byte == 0x01 || (byte >= 0xD0 && byte <= 0xD8)) {
        state = State::JpegFf;
        return true;
      }
      if (byte == 0xD9 || byte == 0xDA) {
        state = State::Failed;
        return false;
      }
      sofPending = isJpegStartOfFrame(byte);
      state = State::JpegLenHi;
      return true;

    case State::JpegLenHi:
      segmentLength = static_cast<uint16_t>(byte << 8);
      state = State::JpegLenLo;
      return true;

    case State::JpegLenLo:
      segmentLength = static_cast<uint16_t>(segmentLength | byte);
      if (segmentLength < 2 || (sofPending && segmentLength < 7)) {
        state = State::Failed;
        return false;
      }
      if (sofPending) {
        sofByteCount = 0;
        state = State::JpegSof;
      } else if (segmentLength == 2) {
        state = State::JpegFf;
      } else {
        skipLeft = static_cast<uint32_t>(segmentLength) - 2;
        state = State::JpegSkip;
      }
      return true;

    case State::JpegSkip:
      if (--skipLeft == 0) state = State::JpegFf;
      return true;

    case State::JpegSof:
      sofBytes[sofByteCount++] = byte;
      if (sofByteCount == sizeof(sofBytes)) {
        height = static_cast<uint16_t>((sofBytes[1] << 8) | sofBytes[2]);
        width = static_cast<uint16_t>((sofBytes[3] << 8) | sofBytes[4]);
        state = State::Done;
        return false;
      }
      return true;

    case State::Done:
    case State::Failed:
      return false;
  }
  return false;
}

size_t ImageDimsProbe::write(const uint8_t byte) { return feed(byte) ? 1 : 0; }

size_t ImageDimsProbe::write(const uint8_t* data, const size_t len) {
  for (size_t index = 0; index < len; index++) {
    if (!feed(data[index])) return index;
  }
  return len;
}

bool ImageDimsProbe::getDimensions(ImageDimensions& out) const {
  if (state != State::Done || width == 0 || height == 0 || width > INT16_MAX || height > INT16_MAX) {
    return false;
  }
  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}
