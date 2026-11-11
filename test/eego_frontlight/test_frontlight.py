#!/usr/bin/env python3
"""Exercise CrossInk HAL and the pinned SDK's real optional A4 light backend.

I2C/GPIO are modeled; this does not validate a physical unit's wiring or curve.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

def run(cxx):
    sdk = ROOT / 'freeink-sdk/libs/hardware'
    with tempfile.TemporaryDirectory(prefix='eego-frontlight-') as directory:
        binary = str(Path(directory) / 'test')
        subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-DFREEINK_DEVICE_EEGO_A4=1',
                        '-I' + str(HERE / 'stubs'),
                        '-I' + str(sdk / 'FrontlightManager/test/host/stubs'),
                        '-I' + str(sdk / 'FrontlightManager/include'),
                        '-I' + str(sdk / 'BoardConfig/include'),
                        '-I' + str(ROOT / 'lib/hal'),
                        str(HERE / 'test_frontlight.cpp'),
                        str(ROOT / 'lib/hal/HalFrontlight.cpp'),
                        str(sdk / 'FrontlightManager/src/FrontlightManager.cpp'),
                        '-o', binary], check=True)
        for uid in ('BB0408', 'BB040C', 'BB0418', 'BB0409', 'BB0417', 'BB0407', 'BB0419', '123456'):
            for fitted in ('fitted', 'absent'):
                subprocess.run([binary, uid, fitted], check=True)
    # Keep the application startup/sleep ordering contract under regression.
    startup = (ROOT / 'src/main.cpp').read_text().split('void setup() {', 1)[1]
    assert startup.index('Frontlight.initializeHardware();') < startup.index('SETTINGS.loadFromFile();')
    power = (ROOT / 'lib/hal/HalPowerManager.cpp').read_text().split('void HalPowerManager::startDeepSleep', 1)[1]
    assert power.index('Frontlight.setOn(false);') < power.index('gpio.prepareForDeepSleep()')
    print('PASS: 16 fitted/absent UID scenarios and startup/sleep ordering')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    run(parser.parse_args().cxx)
