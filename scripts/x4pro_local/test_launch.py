"""Build-and-launch contracts: current source, device identity, failures and isolation."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch, MagicMock

import launch
from core import Native, compose, network_option
import test_core


class DeviceComposition(unittest.TestCase):
    setUp = test_core.Composition.setUp
    tearDown = test_core.Composition.tearDown

    def test_c3_uses_own_build_assets_and_rejects_s3(self):
        for name in ('bootloader.bin', 'firmware.bin'):
            path = self.build / name
            data = bytearray(path.read_bytes())
            struct.pack_into('<H', data, 12, 5)
            path.write_bytes(data)
        manifest = compose(self.build, self.root / 'c3.bin', chip=5)
        self.assertEqual(manifest['inputs']['firmware.bin']['offset'], 0x10000)
        with self.assertRaisesRegex(ValueError, 'ESP32-S3 bootloader'):
            compose(self.build, self.root / 'wrong.bin')



class Launcher(unittest.TestCase):
    def test_network_uses_runtime_with_wifi_and_dma_bounds_fixes(self):
        for device in launch.DEVICES:
            offline = launch.device_profile(device)
            online = launch.device_profile(device, network=True)
            self.assertEqual(offline['release'], '0.5.0')
            self.assertEqual(online['release'], '0.8.1')
            self.assertNotEqual(offline['binary_sha256'], online['binary_sha256'])
            self.assertIn('/0.8.1/', str(launch.runtime_path(device, network=True)))

    def test_devices_use_matching_chip_environment_and_machine(self):
        for device, target, env in [('x4-pro', 'xtensa', 'x4-pro-debug'),
                                    ('x3', 'riscv32', 'debug'), ('x4', 'riscv32', 'debug')]:
            profile = launch.device_profile(device)
            self.assertEqual(profile['binary'], 'qemu-system-' + target)
            self.assertEqual(profile['environment'], env)
            self.assertTrue(profile['machine'].startswith(device.replace('-', '')))

    def test_unsupported_devices_fail_before_source_or_build(self):
        with patch.object(launch, 'source_root') as source:
            for device in launch.UNSUPPORTED:
                with self.assertRaisesRegex(ValueError, 'no matching hardware model'):
                    launch.main([device])
            source.assert_not_called()

    def test_existing_dependency_checkout_is_not_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '.gitmodules').write_text('[submodule "sdk"]\npath = freeink-sdk\nurl = unused\n')
            (root / 'freeink-sdk').mkdir()
            (root / 'freeink-sdk/.git').write_text('gitdir: existing')
            with patch.object(launch, 'command', return_value=MagicMock(stdout=b'submodule.sdk.path freeink-sdk\n')):
                with patch.object(launch.subprocess, 'run') as run:
                    launch.initialize_dependencies(root)
                    run.assert_not_called()

    def test_c3_readiness_recognizes_hardware_detect_and_rejects_wrong_board(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for device, refresh in [('x3', 'X3_DRF'), ('x4', 'refresh')]:
                native = Native(root, root, profile=launch.device_profile(device))
                serial = root / 'serial.log'
                serial.write_text(f'Hardware detect: {device.upper()}\nEntering activity: Home\nWait complete: {refresh}\n')
                with patch.object(native, 'shot'):
                    native.ready(timeout=1)
                serial.write_text(f'Hardware detect: WRONG\nEntering activity: Home\nWait complete: {refresh}\n')
                with patch.object(native, 'shot'), self.assertRaisesRegex(AssertionError, 'board'):
                    native.ready(timeout=1)

    def test_network_defaults_and_localhost_forwarding(self):
        self.assertEqual(network_option(), "none")
        self.assertEqual(network_option(True), "user,model=esp32_wifi")
        self.assertEqual(network_option(True, 18080),
                         "user,model=esp32_wifi,hostfwd=tcp:127.0.0.1:18080-:80")
        for enabled, port in [(False, 18080), (True, 0), (True, 65536)]:
            with self.assertRaises(ValueError):
                network_option(enabled, port)

    def exercise(self, exit_code, extra=None):
        binary_hash = launch.device_profile('x4-pro', network='--network' in (extra or []))['binary_sha256']
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'freeink-sdk/libs/hardware').mkdir(parents=True)
            fake = MagicMock()
            fake.wait.return_value = exit_code
            with patch.object(launch.platform, 'system', return_value='Darwin'), \
                 patch.object(launch.platform, 'machine', return_value='arm64'), \
                 patch.object(launch.Path, 'home', return_value=root), \
                 patch.object(launch.Path, 'is_file', return_value=True), \
                 patch.object(launch, 'sha', return_value=binary_hash), \
                 patch.object(launch.shutil, 'which', return_value='/tool'), \
                 patch.object(launch, 'source_root', return_value=root), \
                 patch.object(launch, 'initialize_dependencies'), \
                 patch.object(launch, 'install_signal_handlers'), \
                 patch.object(launch, 'stop_owned_group') as stop, \
                 patch.object(launch.subprocess, 'Popen', return_value=fake) as build, \
                 patch.object(launch.os, 'execv') as execute:
                result = launch.main(['x4-pro', '--source', str(root)] + (extra or []))
                argv = build.call_args.args[0]
                self.assertEqual(argv[argv.index('--source') + 1], str(root))
                self.assertEqual(argv[argv.index('--env') + 1], 'x4-pro-debug')
                self.assertTrue(build.call_args.kwargs['start_new_session'])
                stop.assert_called_once_with(fake, grace=10)
                if exit_code:
                    execute.assert_not_called()
                    self.assertEqual(result, exit_code)
                else:
                    runtime_argv = execute.call_args.args[1]
                    if extra:
                        self.assertIn("--network", runtime_argv)
                        self.assertEqual(runtime_argv[runtime_argv.index("--http-port") + 1], "18080")
                    self.assertEqual(runtime_argv[runtime_argv.index('--build-dir') + 1],
                                     argv[argv.index('--output-dir') + 1])
                    self.assertNotEqual(runtime_argv[runtime_argv.index('--build-dir') + 1],
                                        str(root / '.pio/build/x4-pro-debug'))

    def test_network_options_reach_native_runner(self):
        self.exercise(0, ["--network", "--http-port", "18080"])

    def test_build_failure_never_launches_cached_image(self):
        self.exercise(1)

    def test_launch_uses_private_artifacts_from_requested_source(self):
        self.exercise(0)


if __name__ == '__main__':
    unittest.main()
