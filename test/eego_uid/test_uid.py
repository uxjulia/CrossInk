#!/usr/bin/env python3
"""Check the production EEGO factory UID reader and HAL presentation without hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="eego-uid-") as tmp:
    work = Path(tmp)
    (work / "BoardConfig.h").write_text("#pragma once\n")
    (work / "Logging.h").write_text(
        "#pragma once\ninline int errors = 0;\n#define LOG_ERR(...) (++errors)\n")
    (work / "esp_mac.h").write_text('''#pragma once
#include <cstdint>
#include <cstring>
constexpr int ESP_OK = 0;
inline int macResult = ESP_OK;
inline uint8_t factoryMac[6] = {0xAA, 0xBB, 0xCC, 0xBB, 0x04, 0x08};
inline int macReads = 0;
inline int esp_efuse_mac_get_default(uint8_t* out) {
  ++macReads;
  if (macResult != ESP_OK) return macResult;
  memcpy(out, factoryMac, sizeof(factoryMac));
  return ESP_OK;
}
''')
    (work / "test.cpp").write_text('''#include <HalDeviceInfo.h>
#include <Logging.h>
#include <esp_mac.h>
#include <cassert>
#include <cstring>
int main() {
  char uid[7] = "stale";
#if FREEINK_DEVICE_EEGO_A4 && !defined(SIMULATOR)
  assert(HalDeviceInfo::readEegoUid(uid));
  assert(strcmp(uid, "BB0408") == 0);
  factoryMac[0] = 0; factoryMac[1] = 0; factoryMac[2] = 0;
  factoryMac[3] = 0; factoryMac[4] = 0x0A; factoryMac[5] = 0xBC;
  assert(HalDeviceInfo::readEegoUid(uid));
  assert(strcmp(uid, "000ABC") == 0);
  factoryMac[4] = 0; factoryMac[5] = 0;
  assert(HalDeviceInfo::readEegoUid(uid));
  assert(strcmp(uid, "000000") == 0);
  macResult = -1;
  assert(!HalDeviceInfo::readEegoUid(uid));
  assert(uid[0] == '\\0' && errors == 1 && macReads == 4);
#else
  assert(!HalDeviceInfo::readEegoUid(uid));
  assert(uid[0] == '\\0' && errors == 0 && macReads == 0);
#endif
}
''')
    for flags in (("-DFREEINK_DEVICE_EEGO_A4=1",),
                  ("-DFREEINK_DEVICE_EEGO_A4=0",),
                  ("-DFREEINK_DEVICE_EEGO_A4=1", "-DSIMULATOR")):
        binary = work / "test"
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(work), "-I" + str(ROOT / "lib/DeviceInfo"),
                        "-I" + str(ROOT / "freeink-sdk/libs/hardware/BoardConfig/include"),
                        str(work / "test.cpp"), str(ROOT / "lib/DeviceInfo/HalDeviceIdentity.cpp"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: EEGO UID byte order, hex formatting/leading zeros, read failure, other devices and simulator")
