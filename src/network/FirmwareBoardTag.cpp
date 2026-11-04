#include "FirmwareBoardTag.h"

#include <BoardConfig.h>

#include <cstring>

// The board name derives from the FREEINK_DEVICE_* build flags so every
// environment (and any fork built from this source) is tagged automatically.
// X3/X4 share one ESP32-C3 compatibility binary and one tag.
#if FREEINK_DEVICE_X4PRO
#define CROSSINK_BOARD_NAME "x4pro"
#elif FREEINK_DEVICE_X4CLASSIC
#define CROSSINK_BOARD_NAME "x4c"
#elif FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
#define CROSSINK_BOARD_NAME "x4"
#elif FREEINK_DEVICE_PICCO
#define CROSSINK_BOARD_NAME "picco"
#elif FREEINK_DEVICE_STICKY
#define CROSSINK_BOARD_NAME "sticky"
#elif FREEINK_DEVICE_PAPERMONO
#define CROSSINK_BOARD_NAME "papermono"
#elif FREEINK_DEVICE_M5PAPER
#define CROSSINK_BOARD_NAME "m5paper"
#elif FREEINK_DEVICE_LILYGO
#define CROSSINK_BOARD_NAME "lilygo"
#elif FREEINK_DEVICE_M5
#define CROSSINK_BOARD_NAME "m5"
#elif FREEINK_DEVICE_MURPHY
#define CROSSINK_BOARD_NAME "murphy"
#elif FREEINK_DEVICE_MURPHY_M4
#define CROSSINK_BOARD_NAME "murphy-m4"
#elif FREEINK_DEVICE_DELINK
#define CROSSINK_BOARD_NAME "delink"
#elif FREEINK_DEVICE_PAPERS3
#define CROSSINK_BOARD_NAME "papers3"
#else
#error "FirmwareBoardTag: no FREEINK_DEVICE_* flag set; cannot derive board name"
#endif

namespace board_tag {

namespace {
// The magic's first character must not recur inside it — the scanner restarts
// a failed match with a single-byte lookback, which is only exact then.
constexpr size_t MAGIC_LEN = sizeof("CROSSPOINT-BOARD-V1:") - 1;
}  // namespace

const char TAG[] = "CROSSPOINT-BOARD-V1:" CROSSINK_BOARD_NAME ";";

const char* boardName() { return TAG + MAGIC_LEN; }
size_t boardNameLen() { return sizeof(TAG) - 1 - MAGIC_LEN - 1; }  // strip magic and ';'

void Scanner::feed(const uint8_t* data, size_t len) {
  if (mismatchFound) return;
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);
    if (capturing) {
      if (c == ';') {
        capturing = false;
        captured[nameLen] = '\0';
        if (nameLen != boardNameLen() || std::memcmp(captured, boardName(), nameLen) != 0) {
          mismatchFound = true;
          return;
        }
      } else if (nameLen < MAX_NAME && c > 0x20 && c < 0x7F) {
        captured[nameLen++] = c;
      } else {
        // Overlong or non-printable: a chance byte collision, not a real tag.
        capturing = false;
      }
      continue;
    }
    if (c == TAG[magicMatched]) {
      if (++magicMatched == MAGIC_LEN) {
        magicMatched = 0;
        capturing = true;
        nameLen = 0;
      }
    } else {
      magicMatched = (c == TAG[0]) ? 1 : 0;
    }
  }
}

}  // namespace board_tag
