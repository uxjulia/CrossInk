"""Build identity detects relevant inputs, including ignored local overrides."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import os

import build as build_helper

sys.path.insert(0, str(Path(__file__).parent))
from build import identity


class BuildInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.paths = ["platformio.ini", "scripts/build_demo.py", "assets/demo.svg",
                      "web/page.html", "boards/test.json"]
        for name in self.paths:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("original\n")
        (self.root / ".gitignore").write_text("platformio.local.ini\nfreeink-sdk/\n")
        sdk = self.root / "freeink-sdk"
        sdk.mkdir()
        (sdk / "source.cpp").write_text("// SDK input\n")
        for directory in (self.root, sdk):
            self.git(directory, "init", "-q")
            self.git(directory, "add", ".")
            self.git(directory, "-c", "user.name=Tests", "-c", "user.email=tests@example.invalid",
                     "commit", "-qm", "fixture")
        self.pio = self.root / "fake-pio"
        self.pio.write_text('#!' + sys.executable + '\n' + """
import json
from pathlib import Path
local = Path('platformio.local.ini')
print(json.dumps({'environment':'x4-pro-debug','local':local.read_text() if local.exists() else ''}))
""")
        self.pio.chmod(0o700)

    def git(self, directory, *args):
        subprocess.run(["git", *args], cwd=directory, check=True, capture_output=True)

    def tearDown(self):
        self.tmp.cleanup()

    def test_scripts_assets_web_boards_and_ignored_config_change_identity(self):
        before = identity(self.root, str(self.pio))
        for name in self.paths:
            with self.subTest(input=name):
                path = self.root / name
                path.write_text("changed\n")
                self.assertNotEqual(identity(self.root, str(self.pio))["input_sha256"][name],
                                    before["input_sha256"][name])
                path.write_text("original\n")
        local = self.root / "platformio.local.ini"
        local.write_text("debug override\n")
        after = identity(self.root, str(self.pio))
        self.assertIn(local.name, after["input_sha256"])
        self.assertNotEqual(after["effective_config_sha256"], before["effective_config_sha256"])

    def test_untracked_firmware_inputs_and_sdk_content_are_recorded(self):
        before = identity(self.root, str(self.pio))
        script = self.root / "scripts/new_build_step.py"
        script.write_text("# new input\n")
        sdk = self.root / "freeink-sdk/source.cpp"
        sdk.write_text("// changed SDK input\n")
        after = identity(self.root, str(self.pio))
        self.assertIn("scripts/new_build_step.py", after["untracked"])
        self.assertIn("scripts/new_build_step.py", after["input_sha256"])
        self.assertNotEqual(after["sdk_diff"], before["sdk_diff"])

    def test_isolated_build_preserves_standard_dependency_paths_and_freezes_artifacts(self):
        self.pio.write_text('#!' + sys.executable + '\n' + """
import json, os, sys
from pathlib import Path
if 'run' in sys.argv:
    assert 'PLATFORMIO_LIBDEPS_DIR' not in os.environ
    build = Path(os.environ['PLATFORMIO_BUILD_DIR']) / 'debug'
    build.mkdir(parents=True)
    for name in ('firmware.bin', 'bootloader.bin', 'partitions.bin'):
        (build / name).write_bytes(b'new-current-firmware')
else:
    print(json.dumps({'build_dir':os.environ.get('PLATFORMIO_BUILD_DIR')}))
""")
        build_root = self.root / '.pio/emulator-c3'
        frozen = self.root / 'frozen'
        script = Path(__file__).with_name('build.py')
        argv = [str(script), '--source', str(self.root), '--env', 'debug',
                '--pio', str(self.pio), '--log', str(self.root / 'build.log'),
                '--build-root', str(build_root), '--output-dir', str(frozen)]
        with patch.object(sys, 'argv', argv), patch.dict(os.environ), \
             patch.object(build_helper.fcntl, 'flock'), \
             patch.object(build_helper, 'install_signal_handlers'), \
             patch.object(build_helper, 'ignore_termination_signals'):
            build_helper.main()
        record = json.loads((frozen / 'crossink-build.json').read_text())
        self.assertEqual(record['environment'], 'debug')
        (build_root / 'build/debug/firmware.bin').write_bytes(b'later-build')
        self.assertEqual((frozen / 'firmware.bin').read_bytes(), b'new-current-firmware')
        self.assertEqual(record['source']['commit'],
                         subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=self.root).decode().strip())


if __name__ == "__main__":
    unittest.main()
