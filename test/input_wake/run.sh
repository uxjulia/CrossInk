#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/crossink-input-wake.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for device in X4PRO X4CLASSIC STICKY X4 X3; do
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -DARDUINO_ARCH_ESP32=1 \
    -DFREEINK_CAP_TOUCH=0 -DFREEINK_DEVICE_${device}=1 \
    -Istubs -I../../lib/hal -I../../freeink-sdk/libs/hardware/InputManager/include \
    -I../../freeink-sdk/libs/hardware/BoardConfig/include \
    InputWakeTest.cpp ../../lib/hal/InputWake.cpp \
    ../../freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp -o "$BUILD_DIR/test_$device"
  for scenario in normal shared-service signal-failure service-failure partial-attach-failure partial-type-failure touch-attach-failure other-touch-controller toggle timeout-wrap; do
    "$BUILD_DIR/test_$device" "$scenario"
  done
done
for mode in native esp32; do
  flags="-DESP_PLATFORM=1"
  scenario="native-fallback"
  if [ "$mode" = esp32 ]; then flags="-DARDUINO_ARCH_ESP32=1 -DCONFIG_IDF_TARGET_ESP32=1"; scenario="original-esp32"; fi
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter $flags \
    -DFREEINK_CAP_TOUCH=0 -DFREEINK_DEVICE_X4PRO=1 \
    -Istubs -I../../lib/hal -I../../freeink-sdk/libs/hardware/InputManager/include \
    -I../../freeink-sdk/libs/hardware/BoardConfig/include \
    InputWakeTest.cpp ../../lib/hal/InputWake.cpp \
    ../../freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp -o "$BUILD_DIR/test_$mode"
  "$BUILD_DIR/test_$mode" "$scenario"
done
