"""CrossInk-owned, offline x4prosim control. Python standard library only."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PIN = json.loads(Path(__file__).with_name("provenance.json").read_text())
FLASH_SIZE = 16 * 1024 * 1024
ACTIVITY_LOG_SIGNATURE = b"Entering activity: %s"
CRASH = re.compile(r"Guru Meditation|panic(?:'ed|ked)?|abort\(\)|assert failed|Assertion failed|Stack smashing|watchdog.*(?:trigger|reset)|Segmentation fault", re.I)
REFRESH = r"Wait complete:\s+8179_(?:DRF|gray_pre_DRF|gray_split_DRF)"


def installed_defaults(cache=None):
    cache = Path(cache or Path.home() / ".cache/crossink/x4prosim" / PIN["commit"]).resolve()
    result = dict(runtime=str(cache / "native/qemu"), web_archive=str(cache / "browser/x4prosim-wasm.zip"),
                  build_dir=None, output=str(Path.home() / ".local/state/crossink/x4pro-runs"))
    path = cache / "defaults.json"
    if path.exists():
        data = json.loads(path.read_text())
        if data.get("emulator_commit") != PIN["commit"]:
            raise ValueError(f"Installed configuration pin differs from reviewed emulator: {path}")
        for name in ("runtime", "web_archive", "build_dir"):
            if name not in data:
                continue
            value = (cache / data[name]).resolve()
            if Path(data[name]).is_absolute() or not value.is_relative_to(cache):
                raise ValueError(f"Installed {name} must be relative to the pinned cache")
            result[name] = str(value)
    return result


class Terminated(KeyboardInterrupt):
    def __init__(self, signum):
        self.signum = signum


class LogProfileError(ValueError):
    """A healthy production image cannot satisfy debug-only predicates."""


def require_debug_image(firmware):
    if not firmware["log_profile"]["activity_entry_signature_present"]:
        raise LogProfileError("Native Home/scenario predicates require the supported x4-pro-debug image "
            "(LOG_LEVEL=2). This image omits the activity-entry log signature; firmware was not started.")


def install_signal_handlers():
    def terminate(signum, _frame):
        # One signal starts bounded cleanup. Repeated terminal/cancellation
        # signals must not interrupt that cleanup and orphan the owned child.
        ignore_termination_signals()
        raise Terminated(signum)
    for value in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(value, terminate)


def ignore_termination_signals():
    for value in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(value, signal.SIG_IGN)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def command(argv, timeout=30, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, timeout=timeout,
                          capture_output=True, **kwargs)


def stop_owned_group(process, grace=3):
    """Reap the leader and stop its private process group, including orphans."""
    if process is None:
        return
    group = process.pid  # Every caller starts the child with start_new_session=True.

    def alive():
        process.poll()  # Reap our child even when another group member survives.
        try:
            os.killpg(group, 0)
            return True
        except ProcessLookupError:
            return False

    def wait_group():
        end = time.monotonic() + grace
        while alive() and time.monotonic() < end:
            time.sleep(0.05)
        return not alive()

    if alive():
        try:
            os.killpg(group, signal.SIGTERM)
        except ProcessLookupError:
            pass
        if not wait_group():
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
            if not wait_group():
                raise RuntimeError(f"Owned process group {group} remains after bounded SIGKILL cleanup")
    process.wait(timeout=grace)


def compose(build, output, chip=9):
    """Keep CrossInk's own bootloader/table, and validate the app0 bounds."""
    build, output = Path(build).resolve(), Path(output)
    boot = (build / "bootloader.bin").read_bytes()
    table = (build / "partitions.bin").read_bytes()
    app = (build / "firmware.bin").read_bytes()
    if len(boot) < 24 or boot[0] != 0xE9 or struct.unpack_from("<H", boot, 12)[0] != chip:
        raise ValueError(f"Expected an ESP32-{'S3' if chip == 9 else 'C3'} bootloader")
    if len(app) < 24 or app[0] != 0xE9 or struct.unpack_from("<H", app, 12)[0] != chip:
        raise ValueError(f"Expected an ESP32-{'S3' if chip == 9 else 'C3'} application")
    partitions = []
    for offset in range(0, len(table) - 31, 32):
        magic, kind, subtype, start, size, label, flags = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50AA:
            break
        partitions.append(dict(kind=kind, subtype=subtype, start=start, size=size,
                               label=label.split(b"\0")[0].decode("ascii")))
    app0 = next((p for p in partitions if p["kind"] == 0 and p["subtype"] == 0x10), None)
    if not app0 or app0["start"] != 0x10000 or len(app) > app0["size"]:
        raise ValueError("Missing app0 at 0x10000, or application exceeds its partition")
    if len(boot) > 0x8000 or len(table) > 0x1000:
        raise ValueError("Boot assets overlap the partition table or NVS")
    for p in partitions:
        if p["start"] < 0x9000 or p["start"] + p["size"] > FLASH_SIZE:
            raise ValueError("Partition outside the 16 MiB flash")
    image = bytearray(b"\xff" * FLASH_SIZE)
    inputs = {}
    for name, offset, data in [("bootloader.bin", 0, boot), ("partitions.bin", 0x8000, table),
                               ("firmware.bin", 0x10000, app)]:
        image[offset:offset + len(data)] = data
        inputs[name] = dict(path=str(build / name), sha256=sha(build / name), size=len(data), offset=offset)
    record_path = build / "crossink-build.json"
    record = json.loads(record_path.read_text()) if record_path.exists() else None
    if record is not None:
        for name, info in inputs.items():
            expected = record.get("artifacts", {}).get(name)
            if isinstance(expected, dict):
                expected = expected.get("sha256")
            if expected != info["sha256"]:
                raise ValueError(f"Build record artifact SHA-256 missing or mismatched for {name}; source attribution rejected")
    output.write_bytes(image)
    return dict(build_dir=str(build), inputs=inputs, partitions=partitions,
                flash_sha256=sha(output), ota_metadata="erased, boot app0",
                log_profile=dict(activity_entry_signature_present=ACTIVITY_LOG_SIGNATURE in app,
                    signature=ACTIVITY_LOG_SIGNATURE.decode(), predicates_require="x4-pro-debug / LOG_LEVEL=2"),
                build_record=record,
                build_record_verification="artifact-hashes-match" if record is not None else "no-record")


def synthetic_epub(path):
    """Original public-domain test text; stable ZIP timestamps and no network."""
    text = "".join(f'<p>Paragraph {i}. The quiet reader follows a long path through a garden. '
                   'Every page offers another place to pause and continue reading.</p>' for i in range(1, 121))
    files = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        "OEBPS/content.opf": '<package version="2.0" unique-identifier="id" xmlns="http://www.idpf.org/2007/opf"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">crossink-test-001</dc:identifier><dc:title>CrossInk Test Book</dc:title><dc:creator>CrossInk tests</dc:creator><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="chapter"/></spine></package>',
        "OEBPS/chapter.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Test chapter</title></head><body><h1>CrossInk Test Book</h1>' + text + '</body></html>',
        "OEBPS/toc.ncx": '<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/"><head><meta name="dtb:uid" content="crossink-test-001"/></head><docTitle><text>CrossInk Test Book</text></docTitle><navMap><navPoint id="p1" playOrder="1"><navLabel><text>Test chapter</text></navLabel><content src="chapter.xhtml"/></navPoint></navMap></ncx>',
    }
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def fixture(destination, name, network=False):
    if name not in ("blank-home", "reading", "sd-assets"):
        raise ValueError("Unknown disposable fixture")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    tree = destination / "files"
    tree.mkdir()
    if network:
        wifi = tree / ".crosspoint/wifi.json"
        wifi.parent.mkdir(parents=True, exist_ok=True)
        write_json(wifi, dict(lastConnectedSsid="PICSimLabWifi", credentials=[dict(ssid="PICSimLabWifi", password="")]))
    if name != "blank-home":
        synthetic_epub(tree / "TestBook.epub")
    if name == "sd-assets":
        languages = tree / ".crossink/languages"
        languages.mkdir(parents=True)
        (languages / "test.yaml").write_text('_language_code: "ZZ-TEST"\n_language_name: "Local Test"\n_direction: "ltr"\n_keyboard: "EN"\nSTR_SETTINGS_TITLE: "Test settings"\n')
        # Package the existing maintained/licensed generated French patterns.
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        from package_hyphenation import pack_bytes
        packs = tree / ".crossink/hyphenation"
        packs.mkdir()
        (packs / "hyph-fr.cphyph").write_bytes(pack_bytes("fr"))
    image = destination / "sd.img"
    with image.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
        stream.seek(446)
        stream.write(struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", 2048, 64 * 2048 - 2048))
        stream.seek(510)
        stream.write(b"\x55\xaa")
    mkfs = shutil.which("mkfs.fat") or "/opt/homebrew/sbin/mkfs.fat"
    command([mkfs, "--invariant", "-F", "32", "--offset", "2048", image, "64512"])
    children = sorted(tree.iterdir())
    if children:
        for entry in [*tree.rglob("*"), tree]:
            os.utime(entry, (1767225600, 1767225600))  # 2026-01-01 UTC.
        command(["mcopy", "-m", "-s", "-i", str(image) + "@@1048576", *children, "::"],
                env=dict(os.environ, TZ="UTC"))
    write_json(destination / "fixture.json", dict(name=name, sha256=sha(image),
        files={str(p.relative_to(tree)): sha(p) for p in sorted(tree.rglob("*")) if p.is_file()},
        text_license="Synthetic test EPUB text: CC0-1.0",
        hyphenation_license="See assets/hyphenation/ notices in this CrossInk source"))
    return image


class Budget:
    def __init__(self, seconds):
        self.end = time.monotonic() + seconds

    def remaining(self, maximum=None):
        left = self.end - time.monotonic()
        if left <= 0:
            raise TimeoutError("Overall run deadline exceeded")
        return min(left, maximum) if maximum else left


class QMP:
    def __init__(self, path, budget, events):
        self.sock = socket.socket(socket.AF_UNIX)
        self.budget, self.events, self.counter = budget, events, 0
        self.sock.settimeout(budget.remaining(5))
        try:
            self.sock.connect(str(path))
            self.buffer = b""
            if "QMP" not in self.receive():
                raise RuntimeError("Missing QMP greeting")
            self.call("qmp_capabilities")
        except BaseException:
            self.sock.close()
            raise

    def receive(self, deadline=None):
        while b"\n" not in self.buffer:
            maximum = min(5, deadline - time.monotonic()) if deadline else 5
            if maximum <= 0:
                raise TimeoutError("QMP action deadline exceeded")
            self.sock.settimeout(self.budget.remaining(maximum))
            data = self.sock.recv(65536)
            if not data:
                raise RuntimeError("QMP disconnected")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        reply = json.loads(line)
        self.events("qmp-reply", reply)
        return reply

    def call(self, execute, arguments=None):
        deadline = time.monotonic() + self.budget.remaining(5)
        self.counter += 1
        request = dict(execute=execute, arguments=arguments or {}, id=self.counter)
        self.events("qmp-command", request)
        self.sock.sendall(json.dumps(request).encode() + b"\n")
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("QMP action deadline exceeded")
            reply = self.receive(deadline)
            if reply.get("id") != self.counter:
                continue
            if "error" in reply:
                raise RuntimeError(f"QMP {execute}: {reply['error']}")
            return reply.get("return")

    def close(self):
        self.sock.close()


def network_option(enabled=False, http_port=None):
    if http_port is not None and (not enabled or not 1 <= http_port <= 65535):
        raise ValueError("HTTP forwarding requires networking and a port from 1 to 65535")
    if not enabled:
        return "none"
    value = "user,model=esp32_wifi"
    if http_port is not None:
        value += f",hostfwd=tcp:127.0.0.1:{http_port}-:80"
    return value


class Native:
    def __init__(self, runtime, run, overall=300, action=30, display=False, profile=None, network=False, http_port=None):
        self.profile = profile
        self.network_option = network_option(network, http_port)
        self.run, self.runtime = Path(run), Path(runtime).resolve()
        self.budget, self.action = Budget(overall), action
        self.proc = self.qmp = None
        self.handles = []
        self.socket_dir = None
        self.touch_path = None
        self.display = display

    def event(self, kind, data):
        with (self.run / "events.jsonl").open("a") as stream:
            stream.write(json.dumps(dict(time=time.time(), kind=kind, data=data)) + "\n")

    def start(self):
        profile = self.profile or dict(binary="qemu-system-xtensa", binary_sha256=PIN["native_macos_arm64"]["binary_sha256"], machine="x4pro", shift=2)
        binary = self.runtime / "bin" / profile["binary"]
        if sha(binary) != profile["binary_sha256"]:
            raise ValueError("Native binary differs from the reviewed macOS arm64 pin")
        self.socket_dir = tempfile.TemporaryDirectory(prefix="ciq-", dir="/tmp")
        sock = Path(self.socket_dir.name) / "qmp"
        argv = [str(binary), "-L", str(self.runtime / "share/qemu"), "-machine", profile["machine"],
                "-icount", f"shift={profile['shift']},sleep=on", "-display", "sdl,show-cursor=on" if self.display else "none",
                "-serial", "null", "-drive", f"file={self.run / 'flash.bin'},if=mtd,format=raw",
                "-drive", f"file={self.run / 'sd.img'},if=sd,format=raw", "-nic", self.network_option,
                "-chardev", f"file,id=cdc,path={self.run / 'serial.log'}",
                "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
                "-qmp", f"unix:{sock},server=on,wait=off"]
        self.event("launch", dict(argv=argv, binary_sha256=sha(binary), timing=f"model shift={profile['shift']},sleep=on"))
        write_json(self.run / "control.json", dict(qmp=str(sock), pid=None))
        out, err = (self.run / "stdout.log").open("wb"), (self.run / "stderr.log").open("wb")
        self.handles = [out, err]
        self.proc = subprocess.Popen(argv, cwd=self.run, stdout=out, stderr=err, start_new_session=True)
        write_json(self.run / "control.json", dict(qmp=str(sock), pid=self.proc.pid))
        self.until(lambda: sock.exists(), "QMP socket", 10)
        self.qmp = QMP(sock, self.budget, self.event)

    def health(self):
        self.budget.remaining()
        if self.proc and self.proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with {self.proc.returncode}")
        log = self.logs()
        match = CRASH.search(log)
        if match:
            raise RuntimeError(f"Guest crash: {match.group(0)}")

    def logs(self):
        return "\n".join(p.read_text(errors="replace") if p.exists() else ""
                         for p in (self.run / "serial.log", self.run / "stderr.log"))

    def serial(self):
        path = self.run / "serial.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def mark(self):
        return len(self.serial())

    def until(self, predicate, description, timeout=None):
        end = time.monotonic() + self.budget.remaining(timeout or self.action)
        while time.monotonic() < end:
            self.health()
            value = predicate()
            if value:
                self.event("assertion", dict(description=description, passed=True))
                return value
            time.sleep(min(0.1, self.budget.remaining()))
        raise TimeoutError(f"Timed out waiting for {description}")

    def wait_log(self, pattern, after=0, timeout=None):
        return self.until(lambda: re.search(pattern, self.serial()[after:]), pattern, timeout)

    def ready(self, after=0, timeout=90):
        profile = self.profile or dict(refresh=REFRESH, board="xteink_x4_pro")
        self.wait_log(r"Entering activity: Home[\s\S]*" + profile["refresh"], after, timeout)
        board_log = profile.get("board_log", re.escape("Device: " + profile["board"]) + r"(?:\s|$)")
        if not re.search(board_log, self.logs()):
            raise AssertionError("CrossInk did not report its " + profile["board"] + " board")
        self.shot("home" if not after else f"home-{after}")

    def shot(self, name):
        path = self.run / f"{name}.png"
        self.qmp.call("screendump", dict(filename=str(path), device="panel", format="png"))
        return path

    def press(self, key, milliseconds=150):
        if key not in ("up", "down", "power"):
            raise ValueError("X4 Pro keys: up, down, power")
        self.qmp.call("qom-set", dict(path="/machine/x4pro-keys", property=key, value=True))
        try:
            time.sleep(min(milliseconds / 1000, self.budget.remaining()))
        finally:
            self.qmp.call("qom-set", dict(path="/machine/x4pro-keys", property=key, value=False))

    def find_touch(self):
        def visit(path, depth=0):
            if depth > 12:
                return None
            for item in self.qmp.call("qom-list", dict(path=path)):
                if item["type"].startswith("child<"):
                    child = path.rstrip("/") + "/" + item["name"]
                    if item["type"] == "child<gt911>":
                        return child
                    found = visit(child, depth + 1)
                    if found:
                        return found
            return None
        self.touch_path = self.touch_path or visit("/machine")
        if not self.touch_path:
            raise RuntimeError("GT911 missing from pinned machine")
        return self.touch_path

    def tap(self, x=None, y=None):
        if x is not None and not (0 <= x < 480 and 0 <= y < 800):
            raise ValueError("Touch coordinates must be in the X4 Pro portrait panel")
        self.qmp.call("qom-set", dict(path=self.find_touch(), property="tap",
                                     value="home" if x is None else f"{x},{y}"))

    def refreshed(self, after):
        self.wait_log(REFRESH, after)

    def tap_refresh(self, x, y):
        after = self.mark()
        self.tap(x, y)
        self.refreshed(after)

    def sd_file(self, path):
        # Pause guest during host reads to avoid torn FAT data. Never mount the
        # card on the host or edit it while the firmware holds file handles.
        self.qmp.call("stop")
        try:
            result = subprocess.run(["mtype", "-i", str(self.run / "sd.img") + "@@1048576", "::" + path],
                capture_output=True, timeout=self.budget.remaining(5))
            if result.returncode:
                raise FileNotFoundError(path)
            return result.stdout
        finally:
            self.qmp.call("cont")

    def settings(self):
        return json.loads(self.sd_file("/.crosspoint/crossink-settings.json"))

    def close(self):
        try:
            if self.proc and self.proc.poll() is None:
                try:
                    if self.qmp:
                        self.qmp.budget = Budget(5)
                        self.qmp.call("quit")
                    self.proc.wait(timeout=3)
                except (Exception, KeyboardInterrupt):
                    pass
            stop_owned_group(self.proc)
        finally:
            if self.qmp:
                self.qmp.close()
            for handle in self.handles:
                handle.close()
            if self.socket_dir:
                self.socket_dir.cleanup()
            self.event("teardown", dict(returncode=self.proc.returncode if self.proc else None))
