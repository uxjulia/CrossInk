"""Picco stock recovery must survive build-size checks and serial uploads."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
with contextlib.redirect_stderr(io.StringIO()):
    spec = importlib.util.spec_from_file_location('firmware_size', ROOT / 'scripts/check_firmware_size.py')
    firmware_size = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(firmware_size)


class PiccoBuildTest(unittest.TestCase):
    def test_picco_uses_stock_application_instead_of_recovery_limit(self):
        limit, labels = firmware_size._get_app_partition_limit(ROOT / 'partitions_picco.csv', 'app')
        self.assertEqual(limit, 0xE70000)
        self.assertEqual(labels, 'app')

    def test_unselected_layout_still_checks_every_app_slot(self):
        limit, labels = firmware_size._get_app_partition_limit(ROOT / 'partitions.csv')
        self.assertIn('ota_0', (ROOT / 'partitions.csv').read_text())
        self.assertGreater(limit, 0)
        self.assertGreaterEqual(len(labels.split(', ')), 2)

    def test_missing_selected_partition_fails_closed(self):
        self.assertEqual(firmware_size._get_app_partition_limit(ROOT / 'partitions_picco.csv', 'absent'),
                         (None, None))

    def test_smaller_ota_slot_remains_the_limit(self):
        with tempfile.NamedTemporaryFile(mode='w+', suffix='.csv') as table:
            table.write('first,app,ota_0,0x10000,4M\nsecond,app,ota_1,0x410000,2M\n')
            table.flush()
            self.assertEqual(firmware_size._get_app_partition_limit(table.name)[0], 2 * 1024 * 1024)

    def run_upload_script(self, label='app'):
        class Env(dict):
            def Replace(self, **kwargs):
                self.update(kwargs)
            def GetProjectOption(self, key, default=None):
                return {'board_build.partitions': 'partitions_picco.csv',
                        'custom_firmware_app_partition': label}.get(key, default)
            def subst(self, value):
                return value.replace('$BUILD_DIR', '/build')
        env = Env(PROJECT_DIR=str(ROOT),
                  FLASH_EXTRA_IMAGES=[('0x0', '$BUILD_DIR/bootloader.bin'),
                                      ('0x8000', '$BUILD_DIR/partitions.bin'),
                                      ('0xe000', '/sdk/boot_app0.bin')],
                  UPLOADERFLAGS=['--chip', 'esp32s3', 'write-flash', '-z',
                                 '0x0', '/build/bootloader.bin',
                                 '0x8000', '/build/partitions.bin',
                                 '0xe000', '/sdk/boot_app0.bin'],
                  ESP32_APP_OFFSET=None)
        script = ROOT / 'scripts/app_only_upload.py'
        exec(compile(script.read_text(), str(script), 'exec'), {'Import': lambda _: None, 'env': env})
        return env

    def test_app_upload_removes_materialized_bootloader_and_partition_arguments(self):
        env = self.run_upload_script()
        self.assertEqual(env['FLASH_EXTRA_IMAGES'], [])
        self.assertEqual(env['UPLOADERFLAGS'], ['--chip', 'esp32s3', 'write-flash', '-z'])
        self.assertEqual(env['ESP32_APP_OFFSET'], '0xe0000')

    def test_app_upload_rejects_missing_selected_slot(self):
        with self.assertRaises(RuntimeError):
            self.run_upload_script('absent')


if __name__ == '__main__':
    unittest.main()
