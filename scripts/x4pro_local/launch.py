#!/usr/bin/env python3
"""Build the current CrossInk checkout and open an actual device firmware emulator."""
from __future__ import annotations

import argparse
import os
import math
import tempfile
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from core import network_option, Terminated, command, sha, install_signal_handlers, stop_owned_group

RELEASE = "0.5.0"
COMMIT = "2c7443b56b7ddf10ffd8ab0124de6f4babdd1db7"
BINARIES = {
    "xtensa": "2ede2865f2eaca1e23dc2b81c8600ef205f1f06df9d8fb71c17b6797af19c6ed",
    "riscv32": "0005306996f5276d960249db5de99ccad87b6635fe2be7057f6f71c6fd0c8438",
}
# Older Wi-Fi/GDMA models can overwrite guest memory during network/crypto work.
NETWORK_RELEASE = "0.8.1"
NETWORK_COMMIT = "b19981b3633f7b3a87bd3be8118e2af46de9ebdc"
NETWORK_BINARIES = {
    "xtensa": "eda269e520e272bae71945039f97068bbc18b16c8c866fce9e6880530d5f03ec",
    "riscv32": "b5ccd00b3e31e3cf4449dbe185a0f693374594307ef8efc3b9d8688089ae509f",
}
DEVICES = {
    "x4-pro": dict(target="xtensa", environment="x4-pro-debug", machine="x4pro,panel=uc8179", board="xteink_x4_pro", refresh=r"Wait complete:\s+8179_(?:DRF|gray_pre_DRF|gray_split_DRF)"),
    "x3": dict(target="riscv32", environment="debug", machine="x3", board="xteink_x3", board_log=r"(?:Device: xteink_x3(?:_uc8279)?|Hardware detect: X3)(?:\s|$)", refresh=r"Wait complete:\s+(?:X3_DRF|8279_DRF)"),
    "x4": dict(target="riscv32", environment="debug", machine="x4", board="xteink_x4", board_log=r"(?:Device: xteink_x4|Hardware detect: X4)(?:\s|$)", refresh=r"Wait complete:\s+refresh"),
}
UNSUPPORTED = ("sticky", "x4-classic")


def device_profile(device, network=False):
    if device not in DEVICES:
        raise ValueError(f"{device}: eink-qemu {RELEASE} has no matching hardware model; firmware emulation is unavailable")
    info = DEVICES[device]
    binaries = NETWORK_BINARIES if network else BINARIES
    return dict(info, release=NETWORK_RELEASE if network else RELEASE,
                commit=NETWORK_COMMIT if network else COMMIT, binary="qemu-system-" + info["target"],
                binary_sha256=binaries[info["target"]], shift=0)


def runtime_path(device, network=False):
    release = NETWORK_RELEASE if network else RELEASE
    return Path.home() / ".cache/crossink/eink-qemu" / release / DEVICES[device]["target"] / "qemu"


def source_root(path):
    root = Path(command(["git", "rev-parse", "--show-toplevel"], cwd=path).stdout.decode().strip())
    if not (root / "src/main.cpp").is_file() or not (root / "platformio.ini").is_file():
        raise ValueError("Select a CrossInk checkout with --source /path/to/crossink")
    return root


def selected_source(explicit=None):
    if explicit is not None:
        return source_root(explicit)
    if os.environ.get("CROSSINK_SOURCE"):
        return source_root(Path(os.environ["CROSSINK_SOURCE"]))
    try:
        return source_root(Path.cwd())
    except (ValueError, subprocess.CalledProcessError):
        return source_root(Path.home() / "code/crossink")


def initialize_dependencies(source):
    """Initialize missing modules only; preserve existing SDK commits and edits."""
    modules = source / ".gitmodules"
    if not modules.exists():
        return
    result = command(["git", "config", "--file", str(modules), "--get-regexp", r"submodule\..*\.path"], cwd=source)
    for line in result.stdout.decode().splitlines():
        relative = line.split(None, 1)[1]
        child = source / relative
        if not (child / ".git").exists():
            subprocess.run(["git", "submodule", "update", "--init", "--", relative], cwd=source, check=True)
        initialize_dependencies(child)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="?", default="x4-pro", choices=(*DEVICES, *UNSUPPORTED))
    parser.add_argument("--source", type=Path, help="Checkout to build, including uncommitted edits (default: current checkout, then ~/code/crossink)")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--ready-timeout", type=float, default=180)
    parser.add_argument("--overall-timeout", type=float, default=3600)
    parser.add_argument("--headless", action="store_true", help="Diagnostic run without a window")
    parser.add_argument("--network", action="store_true", help="Enable emulated Wi-Fi through host NAT")
    parser.add_argument("--http-port", type=int, help="Forward a localhost port to the device web server (requires --network)")
    args = parser.parse_args(argv)
    network_option(args.network, args.http_port)
    if args.list_devices:
        print("Firmware emulation: " + ", ".join(DEVICES))
        print("No hardware model: " + ", ".join(UNSUPPORTED))
        return 0
    if any(not math.isfinite(t) or t <= 0 for t in (args.ready_timeout, args.overall_timeout)):
        parser.error("Timeouts must be finite and positive")
    profile = device_profile(args.device, args.network)
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("This launcher currently pins macOS Apple Silicon emulator packages")
    source = selected_source(args.source)
    runtime = runtime_path(args.device, args.network)
    binary = runtime / "bin" / profile["binary"]
    if not binary.is_file() or sha(binary) != profile["binary_sha256"]:
        raise ValueError(
            f"Missing or changed pinned emulator: {binary}. Install the matching eInk-QEMU release ({profile['release']})."
        )
    for tool in ("mcopy", "mtype", "mkfs.fat"):
        if not shutil.which(tool):
            raise ValueError("Missing required host tool: " + tool)
    mcu = "s3" if args.device == "x4-pro" else "c3"
    pio = Path.home() / ".local/bin" / ("codex-pio-" + mcu)
    if not pio.is_file():
        raise ValueError("Missing MCU-specific PlatformIO wrapper: " + str(pio))
    initialize_dependencies(source)
    if not (source / "freeink-sdk/libs/hardware").is_dir():
        raise ValueError("freeink-sdk is not initialized")
    scripts = Path(__file__).resolve().parent
    output = Path.home() / ".local/state/crossink/x4pro-runs"
    output.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix=f"build-{args.device}-", dir=output))
    log = artifacts / "build.log"
    print(f"Building {args.device} from {source}\nBuild log: {log}", flush=True)
    install_signal_handlers()
    process = subprocess.Popen([sys.executable, str(scripts / "build.py"), "--source", str(source),
                                "--env", profile["environment"], "--pio", str(pio), "--log", str(log),
                                "--output-dir", str(artifacts), "--build-root", str(source / ".pio" / ("emulator-" + mcu))], start_new_session=True)
    try:
        code = process.wait()
    finally:
        stop_owned_group(process, grace=10)
    if code:
        print(f"Build failed; emulator was not started. See {log}", file=sys.stderr)
        return code
    build = artifacts
    print("Build succeeded. Opening " + args.device + " firmware emulator; Ctrl-C stops it.", flush=True)
    argv = [sys.executable, str(scripts / "run.py"), "manual", "--device", args.device,
            "--release-runtime", "--build-dir", str(build), "--ready-timeout", str(args.ready_timeout),
            "--overall-timeout", str(args.overall_timeout), "--fixture", "reading"]
    if args.network:
        argv.append("--network")
    if args.http_port is not None:
        argv.extend(["--http-port", str(args.http_port)])
    if args.headless:
        argv.append("--headless")
    os.execv(sys.executable, argv)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Terminated as error:
        sys.exit(128 + error.signum)
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Cannot launch emulator: {error}", file=sys.stderr)
        sys.exit(2)
