"""Exercise the real Picco upload hook without starting esptool or using hardware."""
from pathlib import Path
import runpy
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('app_only_upload.py')


class UploadEnv(dict):
    def __init__(self, root, flags, extra, label='app'):
        super().__init__(PROJECT_DIR=str(root), UPLOADERFLAGS=flags, FLASH_EXTRA_IMAGES=extra)
        self.label = label
        self.replaced = False

    def GetProjectOption(self, key, default=None):
        return {'board_build.partitions': 'partitions.csv',
                'custom_firmware_app_partition': self.label}.get(key, default)

    def subst(self, value):
        return value.replace('$BUILD_DIR', '/build with spaces').replace('$BOOT_OFFSET', '0x0')

    def Replace(self, **values):
        self.replaced = True
        self.update(values)


class AppOnlyUploadTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'partitions.csv').write_text(
            '# Name, Type, SubType, Offset, Size\n'
            'app,app,ota_0,0x10000,0x100000\n'
            'vendor_app,app,ota_1,0x210000,0x100000\n')

    def run_hook(self, flags, extra, label='app'):
        env = UploadEnv(self.root, flags, extra, label)
        runpy.run_path(str(SCRIPT), init_globals={'Import': lambda name: None, 'env': env})
        return env

    def test_matching_pairs_keep_app_arguments_and_selected_offset(self):
        flags = ['--chip', 'esp32s3', '0x0', '$BUILD_DIR/boot.bin',
                 '0x8000', 'partition.bin', '0x210000', 'firmware.bin']
        env = self.run_hook(flags, [('$BOOT_OFFSET', '$BUILD_DIR/boot.bin'), ('0x8000', 'partition.bin')],
                            label='vendor_app')
        self.assertEqual(env['UPLOADERFLAGS'], ['--chip', 'esp32s3', '0x210000', 'firmware.bin'])
        self.assertEqual(env['ESP32_APP_OFFSET'], '0x210000')
        self.assertEqual(env['FLASH_EXTRA_IMAGES'], [])
        self.assertEqual(len(flags), 8)  # The original flags list was not mutated.

    def test_duplicate_declarations_and_occurrences_are_removed(self):
        env = self.run_hook(['0x0', 'boot.bin', '0x0', 'boot.bin', '--tail'],
                            [('0x0', 'boot.bin'), ('0x0', 'boot.bin')])
        self.assertEqual(env['UPLOADERFLAGS'], ['--tail'])

    def test_no_extra_images_preserves_flags(self):
        env = self.run_hook(['--chip', 'esp32s3', '0x10000', 'firmware.bin'], [])
        self.assertEqual(env['UPLOADERFLAGS'], ['--chip', 'esp32s3', '0x10000', 'firmware.bin'])
        self.assertEqual(env['ESP32_APP_OFFSET'], '0x10000')

    def test_mismatches_fail_before_replacing_environment(self):
        for flags in (['0x0', 'different.bin'], ['0x1', 'boot.bin'], ['0x0'], []):
            with self.subTest(flags=flags):
                env = UploadEnv(self.root, flags, [('0x0', 'boot.bin')])
                with self.assertRaisesRegex(RuntimeError, 'could not remove declared extra images'):
                    runpy.run_path(str(SCRIPT), init_globals={'Import': lambda name: None, 'env': env})
                self.assertFalse(env.replaced)
                self.assertEqual(env['UPLOADERFLAGS'], flags)
                self.assertEqual(env['FLASH_EXTRA_IMAGES'], [('0x0', 'boot.bin')])

    def test_partial_match_still_fails_before_environment_mutation(self):
        flags = ['0x0', 'boot.bin', '0x8000', 'unexpected-partition.bin']
        env = UploadEnv(self.root, flags, [('0x0', 'boot.bin'), ('0x8000', 'partition.bin')])
        with self.assertRaisesRegex(RuntimeError, 'partition.bin'):
            runpy.run_path(str(SCRIPT), init_globals={'Import': lambda name: None, 'env': env})
        self.assertFalse(env.replaced)
        self.assertEqual(env['UPLOADERFLAGS'], flags)

    def test_missing_app_slot_still_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'one selected app slot'):
            self.run_hook([], [], label='missing')


if __name__ == '__main__':
    unittest.main()
