#include <BoardConfig.h>

#include "HalDeviceInfo.h"

#if FREEINK_DEVICE_EEGO_A4 && !defined(SIMULATOR)
#include <EegoDeviceId.h>
#include <Logging.h>

#include <cstdio>
#endif

namespace HalDeviceInfo {
bool readEegoUid(char (&uid)[9]) {
  uid[0] = '\0';
#if FREEINK_DEVICE_EEGO_A4 && !defined(SIMULATOR)
  uint32_t value = 0;
  if (!freeink::readEegoA4Uid(value)) {
    LOG_ERR("ABOUT", "Failed to read EEGO factory UID");
    return false;
  }
  snprintf(uid, sizeof(uid), "%lu", static_cast<unsigned long>(value));
  return true;
#else
  return false;
#endif
}

}  // namespace HalDeviceInfo
