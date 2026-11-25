"""Host contract tests. These do not stand in for firmware acceptance."""
import json
from pathlib import Path
import socket
import os
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))
from core import ACTIVITY_LOG_SIGNATURE, Budget, LogProfileError, Native, PIN, QMP, compose, fixture, installed_defaults, require_debug_image, sha


class Composition(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        boot, app = bytearray(128), bytearray(256)
        boot[0] = app[0] = 0xe9
        boot[12] = app[12] = 9
        boot[25:30] = b"owned"
        app[64:64 + len(ACTIVITY_LOG_SIGNATURE)] = ACTIVITY_LOG_SIGNATURE
        (self.build / "bootloader.bin").write_bytes(boot)
        (self.build / "firmware.bin").write_bytes(app)
        (self.build / "partitions.bin").write_bytes(struct.pack("<HBBII16sI", 0x50aa, 0, 0x10,
            0x10000, 0x640000, b"app0", 0) + b"\xff" * 32)

    def tearDown(self):
        self.tmp.cleanup()

    def test_own_boot_assets_are_preserved_and_inputs_immutable(self):
        hashes = {p.name: sha(p) for p in self.build.iterdir()}
        output = self.root / "flash.bin"
        manifest = compose(self.build, output)
        image = output.read_bytes()
        self.assertEqual(len(image), 16 * 1024 * 1024)
        self.assertEqual(image[25:30], b"owned")
        self.assertEqual(image[0x9000:0x10000], b"\xff" * 0x7000)
        self.assertEqual(image[0x10000:0x10100], (self.build / "firmware.bin").read_bytes())
        self.assertEqual(hashes, {p.name: sha(p) for p in self.build.iterdir()})
        self.assertEqual(manifest["flash_sha256"], sha(output))

    def test_reject_c3_instead_of_claiming_s3_compatibility(self):
        path = self.build / "firmware.bin"
        image = bytearray(path.read_bytes())
        image[12] = 5
        path.write_bytes(image)
        with self.assertRaisesRegex(ValueError, "ESP32-S3 application"):
            compose(self.build, self.root / "flash.bin")

    def test_reject_missing_or_oversized_app_partition(self):
        table = self.build / "partitions.bin"
        table.write_bytes(struct.pack("<HBBII16sI", 0x50aa, 0, 0x10, 0x10000, 32, b"app0", 0))
        with self.assertRaisesRegex(ValueError, "application exceeds"):
            compose(self.build, self.root / "flash.bin")

    def test_log_profile_rejects_production_without_weakening_home_predicate(self):
        debug = compose(self.build, self.root / "debug.bin")
        require_debug_image(debug)
        path = self.build / "firmware.bin"
        path.write_bytes(path.read_bytes().replace(ACTIVITY_LOG_SIGNATURE, b"\0" * len(ACTIVITY_LOG_SIGNATURE)))
        production = compose(self.build, self.root / "production.bin")
        with self.assertRaisesRegex(LogProfileError, "LOG_LEVEL=2.*firmware was not started"):
            require_debug_image(production)

    def test_stale_build_record_cannot_attribute_changed_firmware_to_source(self):
        record = {"source": {"commit": "original-source"},
            "artifacts": {name: sha(self.build / name)
                          for name in ("bootloader.bin", "partitions.bin", "firmware.bin")}}
        (self.build / "crossink-build.json").write_text(json.dumps(record))
        verified = compose(self.build, self.root / "verified.bin")
        self.assertEqual(verified["build_record_verification"], "artifact-hashes-match")
        app = self.build / "firmware.bin"
        app.write_bytes(app.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "SHA-256.*mismatched.*firmware.bin"):
            compose(self.build, self.root / "stale.bin")
        self.assertFalse((self.root / "stale.bin").exists())

    def test_cancellation_and_expired_deadline_clean_up_subprocess_runner(self):
        runtime = self.root / "runtime"
        binary = runtime / "bin/qemu-system-xtensa"
        binary.parent.mkdir(parents=True)
        binary.write_text('#!' + sys.executable + '\n' + '''
import json, socket, sys, time
from pathlib import Path
args = sys.argv
sock = args[args.index('-qmp') + 1].split(',')[0][5:]
log = args[args.index('-chardev') + 1].split('path=')[1]
Path(log).write_text('Device: xteink_x4_pro\\nEntering activity: Home\\nWait complete: 8179_DRF\\n')
s = socket.socket(socket.AF_UNIX); s.bind(sock); s.listen(1)
c, _ = s.accept(); f = c.makefile('rwb')
f.write(b'{"QMP": {}}\\n'); f.flush()
for line in f:
    request = json.loads(line)
    if request['execute'] == 'screendump':
        Path(request['arguments']['filename']).write_bytes(b'test-panel')
    f.write(json.dumps({'id':request['id'], 'return':{}}).encode() + b'\\n'); f.flush()
    if request['execute'] == 'quit':
        time.sleep(0.2)
        break
f.close(); c.close(); s.close()
''')
        binary.chmod(0o700)
        scripts = Path(__file__).resolve().parent
        driver = self.root / "driver.py"
        driver.write_text('''
import sys
from pathlib import Path
sys.path.insert(0, sys.argv.pop(1))
import core, run
core.PIN['native_macos_arm64']['binary_sha256'] = core.sha(Path(sys.argv[sys.argv.index('--runtime')+1])/'bin/qemu-system-xtensa')
def seed(destination, name, network=False):
    destination.mkdir(parents=True)
    image=destination/'sd.img'; image.write_bytes(b'\\0'*4096)
    core.write_json(destination/'fixture.json',dict(name=name,sha256=core.sha(image)))
    return image
run.fixture = seed
sys.exit(run.main())
''')
        for interrupt in (signal.SIGTERM, signal.SIGHUP, None):
            with self.subTest(signal=interrupt):
                output = self.root / f"runs-{interrupt}"
                argv = [sys.executable, str(driver), str(scripts), "manual", "--runtime", str(runtime),
                    "--build-dir", str(self.build), "--output", str(output), "--fixture", "blank-home",
                    "--overall-timeout", "20" if interrupt else "1"]
                process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                owned_pid = None
                try:
                    end = time.monotonic() + 5
                    ready = []
                    while time.monotonic() < end:
                        ready = list(output.glob("*/home.png")) if output.exists() else []
                        if ready: break
                        time.sleep(0.02)
                    if not ready:
                        if process.poll() is None: process.kill()
                        stdout, stderr = process.communicate(timeout=3)
                        guest = "".join(p.read_text() for p in output.glob("*/stderr.log"))
                        self.fail("Stub runner did not reach Home: " + stdout.decode() + stderr.decode() + guest)
                    directory = ready[0].parent
                    owned_pid = json.loads((directory / "control.json").read_text())["pid"]
                    if interrupt:
                        os.kill(process.pid, interrupt)
                    else:
                        # Cancellation during natural cleanup must not interrupt it.
                        end = time.monotonic() + 3
                        while time.monotonic() < end and not (directory / "final.png").exists():
                            time.sleep(0.01)
                        self.assertTrue((directory / "final.png").exists())
                        os.kill(process.pid, signal.SIGTERM)
                    stdout, stderr = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 128 + interrupt if interrupt else 0, stderr.decode())
                    manifest = json.loads((directory / "manifest.json").read_text())
                    self.assertEqual(manifest["status"], "interrupted" if interrupt else "stopped")
                    self.assertTrue((directory / "final.png").exists())
                    self.assertTrue(manifest["seed_unchanged"])
                    with self.assertRaises(ProcessLookupError): os.kill(owned_pid, 0)
                    sock = json.loads((directory / "control.json").read_text())["qmp"]
                    self.assertFalse(Path(sock).parent.exists())
                finally:
                    if process.poll() is None:
                        process.kill(); process.communicate(timeout=3)
                    elif process.stdout and not process.stdout.closed:
                        process.communicate(timeout=3)
                    if owned_pid:
                        try: os.killpg(owned_pid, signal.SIGKILL)
                        except ProcessLookupError: pass


class Control(unittest.TestCase):
    def test_installed_defaults_are_pinned_relative_and_optional(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp).resolve()
            self.assertIsNone(installed_defaults(cache)["build_dir"])
            config = cache / "defaults.json"
            data = dict(emulator_commit=PIN["commit"], build_dir="firmware/verified-image")
            config.write_text(json.dumps(data))
            self.assertEqual(installed_defaults(cache)["build_dir"], str(cache / "firmware/verified-image"))
            data["build_dir"] = "../outside"
            config.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "relative to the pinned cache"):
                installed_defaults(cache)
            data.update(build_dir="firmware/verified-image", emulator_commit="wrong-pin")
            config.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "pin differs"):
                installed_defaults(cache)

    def test_teardown_kills_orphan_after_group_leader_exits(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            marker = root / "child.pid"
            child = """
import os, signal, sys, time
from pathlib import Path
signal.signal(signal.SIGTERM, signal.SIG_IGN)
Path(sys.argv[1]).write_text(str(os.getpid()))
time.sleep(60)
"""
            leader = """
import subprocess, sys, time
from pathlib import Path
subprocess.Popen([sys.executable, '-c', sys.argv[2], sys.argv[1]],
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while not Path(sys.argv[1]).exists(): time.sleep(0.01)
"""
            process = subprocess.Popen([sys.executable, "-c", leader, str(marker), child],
                start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                self.assertEqual(process.wait(timeout=3), 0)
                child_pid = int(marker.read_text())
                os.kill(child_pid, 0)
                n = Native(root, root)
                n.proc = process
                n.close()
                with self.assertRaises(ProcessLookupError): os.kill(child_pid, 0)
            finally:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.wait(timeout=3)

    def test_qmp_fragmented_events_response_ids_and_errors(self):
        a, b = socket.socketpair()
        q = QMP.__new__(QMP)
        q.sock, q.budget, q.events, q.counter, q.buffer = a, Budget(2), lambda *_: None, 0, b""
        def server():
            stream = b.makefile("rb")
            request = json.loads(stream.readline())
            message = (json.dumps({"event": "RESET"}) + "\n" + json.dumps({"id": 99, "return": {}})
                + "\n" + json.dumps({"id": request["id"], "error": {"desc": "unsupported"}}) + "\n").encode()
            for offset in range(0, len(message), 7):
                b.sendall(message[offset:offset + 7])
            stream.close()
            b.close()
        t = threading.Thread(target=server)
        t.start()
        try:
            with self.assertRaisesRegex(RuntimeError, "unsupported"):
                q.call("bad-command")
        finally:
            q.close()
            t.join(timeout=2)
        self.assertFalse(t.is_alive())

    def test_short_serial_message_not_skipped_by_stderr_cursor(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "serial.log").write_text("boot\n")
            (root / "stderr.log").write_text("extra stderr" * 50)
            n = Native(root, root, overall=2)
            mark = n.mark()
            with (root / "serial.log").open("a") as stream:
                stream.write("Wait complete: 8179_gray_pre_DRF (625 ms)\n")
            n.refreshed(mark)

    def test_crash_and_action_deadline_are_failures(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            n = Native(root, root, overall=2, action=0.1)
            with self.assertRaises(TimeoutError):
                n.until(lambda: False, "unreachable")
            (root / "serial.log").write_text("assert failed: guest state")
            with self.assertRaisesRegex(RuntimeError, "Guest crash"):
                n.health()

    def test_start_failure_can_always_be_torn_down(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "bin/qemu-system-xtensa"
            binary.parent.mkdir()
            binary.write_text("#!/bin/sh\nexit 7\n")
            binary.chmod(0o700)
            with patch.dict(PIN["native_macos_arm64"], {"binary_sha256": sha(binary)}):
                n = Native(root, root, overall=2)
                with self.assertRaisesRegex(RuntimeError, "QEMU exited"):
                    try:
                        n.start()
                    finally:
                        n.close()
                self.assertEqual(n.proc.poll(), 7)
                self.assertFalse(Path(n.socket_dir.name).exists())


if __name__ == "__main__":
    unittest.main()
