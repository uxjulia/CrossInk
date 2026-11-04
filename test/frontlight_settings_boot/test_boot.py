#!/usr/bin/env python3
"""Run production boot order, HAL and gesture validation across fresh processes.

The SDK fixture models I2C presence becoming known only after begin(). Settings
I/O is a small JSON fixture; normalization runs the actual settings source.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def harness():
    settings = (ROOT / 'src/CrossPointSettings.cpp').read_text()
    header = (ROOT / 'src/CrossPointSettings.h').read_text()
    actions = re.search(r'enum TWO_FINGER_SWIPE_ACTION\s*\{.*?\};', header, re.S).group()
    validation_start = settings.index('  if (normalizeTwoFingerSwipeActions(*this))')
    validation = settings[validation_start:settings.index('\n\n', validation_start)]
    startup = (ROOT / 'src/main.cpp').read_text().split('void setup() {', 1)[1].split('void loop()', 1)[0]
    calls = re.findall(r'\b(?:Frontlight\.(?:initializeHardware|begin)|SETTINGS\.loadFromFile)\([^;]*\);', startup)
    # Use the real ordering, so moving detection below load reproduces the loss.
    assert sum('SETTINGS.loadFromFile' in call for call in calls) == 1
    assert sum('Frontlight.begin(' in call for call in calls) == 1
    return r'''
#include <ArduinoJson.h>
#include <cassert>
#include <fstream>
#include <string>
#include "HalFrontlight.h"
#include "src/util/TwoFingerSwipe.h"
struct CrossPointSettings {
''' + actions + r'''
  uint8_t leftEdgeUp = 0, leftEdgeDown = 0, rightEdgeUp = 0, rightEdgeDown = 0;
  uint8_t twoFingerSwipeUp = 0, twoFingerSwipeDown = 0, twoFingerSwipeLeft = 0, twoFingerSwipeRight = 0;
  uint8_t frontlightBrightness = 73, frontlightWarmth = 42;
  std::string path;
  static bool isTwoFingerSwipeActionAvailable(uint8_t, bool, bool);
  static bool normalizeTwoFingerSwipeActions(CrossPointSettings&, uint8_t CrossPointSettings::* = nullptr);
  bool loadFromFile();
} SETTINGS;
''' + function(settings, 'bool CrossPointSettings::isTwoFingerSwipeActionAvailable(') + '\n' + function(
        settings, 'bool CrossPointSettings::normalizeTwoFingerSwipeActions(') + r'''
bool CrossPointSettings::loadFromFile() {
  assert(frontlight_test::writes == 0);  // Detection must not restore the light early.
  JsonDocument doc;
  std::ifstream input(path);
  assert(!deserializeJson(doc, input));
  input.close();
  leftEdgeUp = doc["leftEdgeUp"].as<uint8_t>();
  leftEdgeDown = doc["leftEdgeDown"].as<uint8_t>();
  rightEdgeUp = doc["rightEdgeUp"].as<uint8_t>();
  rightEdgeDown = doc["rightEdgeDown"].as<uint8_t>();
  twoFingerSwipeUp = doc["twoFingerSwipeUp"].as<uint8_t>();
  twoFingerSwipeDown = doc["twoFingerSwipeDown"].as<uint8_t>();
  twoFingerSwipeLeft = doc["twoFingerSwipeLeft"].as<uint8_t>();
  twoFingerSwipeRight = doc["twoFingerSwipeRight"].as<uint8_t>();
  bool needsResave = false;
''' + validation + r'''
  doc["leftEdgeUp"] = leftEdgeUp;
  doc["leftEdgeDown"] = leftEdgeDown;
  doc["rightEdgeUp"] = rightEdgeUp;
  doc["rightEdgeDown"] = rightEdgeDown;
  doc["twoFingerSwipeUp"] = twoFingerSwipeUp;
  doc["twoFingerSwipeDown"] = twoFingerSwipeDown;
  doc["twoFingerSwipeLeft"] = twoFingerSwipeLeft;
  doc["twoFingerSwipeRight"] = twoFingerSwipeRight;
  if (needsResave) {
    std::ofstream output(path);
    serializeJson(doc, output);
  }
  return true;
}
int main(int argc, char** argv) {
  assert(argc == 4);
  const std::string mode = argv[1];
  frontlight_test::i2c = mode != "pwm";
  frontlight_test::fitted = mode != "absent";
  frontlight_test::warmChannel = mode != "mono";
  SETTINGS.path = argv[2];
  const bool restoreLightOn = std::string(argv[3]) == "on";
''' + '\n'.join(calls) + r'''
  assert(frontlight_test::begins == 1);  // Applying settings must not reinitialize the driver.
  assert(Frontlight.present() == frontlight_test::fitted);
  assert(Frontlight.hasColorTemperature() == (frontlight_test::fitted && frontlight_test::warmChannel));
  assert(Frontlight.isOn() == (restoreLightOn && frontlight_test::fitted));
  assert(frontlight_test::brightness == (restoreLightOn && frontlight_test::fitted ? 73 : 0));
  if (frontlight_test::fitted) {
    assert(Frontlight.brightness() == 73 && Frontlight.warmth() == 42);
    Frontlight.setOn(false);
    assert(frontlight_test::brightness == 0);
    Frontlight.setOn(true);
    assert(frontlight_test::brightness == 73);
  } else {
    assert(frontlight_test::writes == 0);
  }
}
'''


def run(cxx, arduinojson):
    with tempfile.TemporaryDirectory(prefix='frontlight-settings-') as directory:
        work = Path(directory)
        source = work / 'boot.cpp'
        source.write_text(harness())
        binary = work / 'boot'
        subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(HERE / 'stubs'), '-I', str(ROOT / 'lib/hal'),
                        '-I', str(ROOT), '-I', arduinojson, str(source),
                        str(ROOT / 'lib/hal/HalFrontlight.cpp'), '-o', str(binary)], check=True)
        header = (ROOT / 'src/CrossPointSettings.h').read_text()
        # Resolve persisted enum IDs from the actual declaration instead of UI positions.
        enum = re.search(r'enum TWO_FINGER_SWIPE_ACTION\s*\{(.*?)\};', header, re.S).group(1)
        names = re.findall(r'^\s*(TWO_FINGER_SWIPE_\w+)', enum, re.M)
        action = {name.removeprefix('TWO_FINGER_SWIPE_'): i for i, name in enumerate(names)}
        fixtures = [
            [action['INCREASE_BRIGHTNESS'], action['DECREASE_BRIGHTNESS'],
             action['INCREASE_WARMTH'], action['DECREASE_WARMTH']],
            [action['BACK_HOME'], action['INCREASE_FONT_SIZE'], action['SELECT_CHAPTER'], action['NEXT_CHAPTER']],
            [action['INCREASE_BRIGHTNESS']] * 4,  # Edge actions may share assignments.
            [255, action['NOT_SET'], action['HOME_READER'], action['DECREASE_FONT_SIZE']],
        ]
        edge_keys = ['leftEdgeUp', 'leftEdgeDown', 'rightEdgeUp', 'rightEdgeDown']
        two_keys = ['twoFingerSwipeUp', 'twoFingerSwipeDown', 'twoFingerSwipeLeft', 'twoFingerSwipeRight']
        boots = 0
        for mode in ('i2c', 'pwm', 'mono', 'absent'):
            for light in ('on', 'off'):
                for values in fixtures:
                    saved = dict(zip(edge_keys, values))
                    saved.update(zip(two_keys, fixtures[0]))
                    path = work / 'settings.json'
                    path.write_text(json.dumps(saved))
                    expected = dict(saved)
                    for key, value in saved.items():
                        unsupported = (value == 255 or
                                       (mode == 'absent' and value in fixtures[0]) or
                                       (mode == 'mono' and value in fixtures[0][2:]))
                        if unsupported:
                            expected[key] = action['NOT_SET']
                    for _ in range(3):
                        subprocess.run([str(binary), mode, str(path), light], check=True)
                        assert json.loads(path.read_text()) == expected, (mode, light, values)
                        boots += 1
        print(f'PASS: {boots} fresh boots preserve supported edge/two-finger actions and light state')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--arduinojson', required=True)
    args = parser.parse_args()
    run(args.cxx, args.arduinojson)
