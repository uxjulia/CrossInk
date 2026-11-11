"""Verify A4 build capabilities, firmware naming and release catalog compatibility.

Run with the Python interpreter that has PlatformIO installed.
"""
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from platformio.project.config import ProjectConfig

ROOT = Path(__file__).resolve().parents[1]
RENAME = runpy.run_path(str(ROOT / 'scripts/rename_firmware.py'))
VERSION = runpy.run_path(str(ROOT / 'scripts/git_branch.py'))


class ArtifactEnv(dict):
    def __init__(self, config, name):
        super().__init__(PROJECT_DIR=str(ROOT), PIOENV=name)
        self.config = config

    def GetProjectOption(self, name):
        return self.config.get('env:' + self['PIOENV'], name)


class EegoA4ReleaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = ProjectConfig(str(ROOT / 'platformio.ini'))

    def test_debug_and_production_capabilities(self):
        for name, level in (('eego_a4', 1), ('eego_a4-debug', 2)):
            with self.subTest(environment=name):
                section = 'env:' + name
                get = lambda key: self.config.get(section, key)
                self.assertEqual(get('board_build.mcu'), 'esp32s3')
                self.assertEqual(get('board_build.flash_mode'), 'dio')
                self.assertEqual(get('board_build.arduino.memory_type'), 'dio_opi')
                self.assertEqual(get('board_build.partitions'), 'partitions.csv')
                self.assertEqual(get('custom_firmware_device_type'), 'eego_a4')
                flags = get('build_flags')
                for define in ('FREEINK_DEVICE_EEGO_A4', 'CROSSINK_APP_CAP_TOUCH',
                               'FREEINK_FB_PSRAM', 'FREEINK_CAP_USB_MSC',
                               'CROSSINK_APP_CAP_USB_DRIVE', 'USE_BLOCK_DEVICE_INTERFACE',
                               'CROSSINK_SCALABLE_FONTS', 'ARDUINO_USB_MODE'):
                    self.assertIn(f'-D{define}=1', flags)
                self.assertIn('-DBOARD_HAS_PSRAM', flags)
                self.assertEqual([f for f in flags if f.startswith('-DLOG_LEVEL=')],
                                 [f'-DLOG_LEVEL={level}'])
                self.assertFalse(any(f.startswith('-DCROSSINK_VERSION=') for f in flags))
                self.assertEqual('-DCROSSPOINT_WAIT_FOR_USB_SERIAL' in flags, level == 2)

    def test_release_and_rc_versions(self):
        env = ArtifactEnv(self.config, 'eego_a4')
        with patch.dict(os.environ, {}, clear=True):
            self.assertTrue(VERSION['get_hardware_version'](str(ROOT), env['PIOENV']).endswith('-eego_a4'))
        with patch.dict(os.environ, {'CROSSINK_RELEASE_VERSION': '2.0.0'}, clear=True):
            self.assertEqual(VERSION['get_hardware_version'](str(ROOT), env['PIOENV']), '2.0.0-eego_a4')
        with patch.dict(os.environ, {'CROSSINK_RELEASE_VERSION': '1', 'CROSSINK_RC_HASH': 'abc1234'}, clear=True):
            expected = VERSION['get_crossink_version'](str(ROOT)) + '-abc1234-RC'
            self.assertEqual(VERSION['get_hardware_version'](str(ROOT), env['PIOENV']), expected)

    def test_assets_and_catalog(self):
        with tempfile.TemporaryDirectory(prefix='crossink-eego-assets-') as directory:
            root = Path(directory)
            # Exercise the real post-build hook for both environments.
            for name in ('eego_a4', 'eego_a4-debug'):
                env = ArtifactEnv(self.config, name)
                build = root / name
                build.mkdir()
                binary = build / 'firmware.bin'
                payload = b'app-CROSSPOINT-BOARD-V1:eego_a4;'
                binary.write_bytes(payload)
                for variables, expected in (
                    ({}, 'firmware-eego_a4.bin'),
                    ({'CROSSINK_RELEASE_VERSION': '2.0.0'}, 'firmware-eego_a4-v2.0.0.bin'),
                    ({'CROSSINK_RELEASE_VERSION': '1', 'CROSSINK_RC_BRANCH': '2.0.0',
                      'CROSSINK_RC_HASH': 'abc1234'}, 'firmware-eego_a4-2.0.0-abc1234-RC.bin'),
                ):
                    with patch.dict(os.environ, variables, clear=True):
                        RENAME['rename_firmware'](None, [binary], env)
                    self.assertEqual((build / expected).read_bytes(), payload)
                stable = build / 'firmware-eego_a4-v2.0.0.bin'
                output = build / 'catalog'
                subprocess.run([sys.executable, str(ROOT / 'scripts/generate_release_catalog.py'),
                                '--version', '2.0.0', '--repo', 'uxjulia/CrossInk',
                                '--firmware', str(stable), '--output', str(output)],
                               check=True, capture_output=True, text=True)
                release, = json.loads(output.read_text())['releases']
                self.assertEqual(release['variant'], 'eego_a4')
                self.assertEqual(release['supported_devices'], ['eego_a4'])
                self.assertEqual(release['size'], len(payload))
                self.assertEqual(release['firmware_sha256'], hashlib.sha256(payload).hexdigest())
                self.assertTrue(release['firmware_url'].endswith('/v2.0.0/' + stable.name))

    def test_workflows_build_and_publish_a4(self):
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertRegex(ci, r'environment: \[[^\n]*\beego_a4, eego_a4-debug\]')
        for workflow in ('release.yml', 'release_candidate.yml'):
            content = (ROOT / '.github/workflows' / workflow).read_text()
            self.assertIn('- environment: eego_a4\n            firmware: firmware-eego_a4', content)
            suffix = ('v${{ env.RELEASE_VERSION }}' if workflow == 'release.yml'
                      else '${{ env.RC_BRANCH_SAFE }}-${{ env.RC_HASH_SHORT }}-RC')
            expected_count = 2 if workflow == 'release.yml' else 1
            self.assertEqual(content.count(f'firmware-eego_a4-{suffix}.bin'), expected_count)
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn('--firmware "release-firmware/firmware-eego_a4-v${{ env.RELEASE_VERSION }}.bin"', release)


if __name__ == '__main__':
    unittest.main()
