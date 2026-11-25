#!/usr/bin/env python3
"""Manual runs and assertion-based firmware acceptance on pinned x4prosim."""
from __future__ import annotations

import argparse
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import shutil
import platform
import signal
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

from core import network_option, ROOT, PIN, Budget, LogProfileError, Native, Terminated, command, compose, fixture, ignore_termination_signals, install_signal_handlers, installed_defaults, require_debug_image, sha, stop_owned_group, write_json


def prepare(args):
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=root))
    os.chmod(run, 0o700)
    print(f"Run evidence: {run}", flush=True)
    firmware = compose(args.build_dir, run / "flash.bin", chip=5 if getattr(args, "device", "x4-pro") in ("x3", "x4") else 9)
    if args.sd:
        seed = Path(args.sd).resolve()
        if args.mode == "test":
            record = json.loads((seed.parent / "fixture.json").read_text())
            if record["name"] != "sd-assets" or record["sha256"] != sha(seed):
                raise ValueError("Acceptance replay needs an unchanged named sd-assets seed")
    else:
        seed = fixture(run / "fixture", args.fixture, network=getattr(args, "network", False))
    shutil.copyfile(seed, run / "sd.img")
    manifest = dict(schema=1, pin=PIN, firmware=firmware,
        fixture=dict(name=args.fixture if not args.sd else "explicit-input", path=str(seed), sha256=sha(seed)),
        device=getattr(args, "device", "x4-pro"), mode=args.mode, backend=args.backend, timing="model shift=2,sleep=on; browser turbo=0",
        network=network_option(getattr(args, "network", False), getattr(args, "http_port", None)), harness_source=dict(path=str(ROOT),
            commit=command(["git", "rev-parse", "HEAD"], cwd=ROOT).stdout.decode().strip(),
            status=command(["git", "status", "--short"], cwd=ROOT).stdout.decode()),
        timeouts=dict(readiness=args.ready_timeout, action=args.action_timeout, overall=args.overall_timeout),
        created=time.time(), status="running", scenarios=[])
    manifest["tools"] = {name: shutil.which(name) for name in ("python3", "mcopy", "mtype", "node")}
    manifest["host"] = dict(platform=platform.platform(), python=sys.version, interpreter=sys.executable)
    if not args.sd:
        manifest["fixture"]["record"] = json.loads((run / "fixture/fixture.json").read_text())
    write_json(run / "manifest.json", manifest)
    return run, manifest


def browser(args, run):
    archive = Path(args.web_archive).resolve()
    if sha(archive) != PIN["browser"]["archive_sha256"]:
        raise ValueError("Browser archive differs from the reviewed pinned CI artifact")
    web = run / "web"
    web.mkdir()
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            target = (web / member.filename).resolve()
            if not target.is_relative_to(web):
                raise ValueError("Archive path escapes private run directory")
        z.extractall(web)
    index = next(web.rglob("index.html"), None)
    if index is None:
        raise ValueError("Pinned archive has no browser package")
    webroot = index.parent
    shutil.copyfile(run / "flash.bin", webroot / "crossink-flash.bin")
    shutil.copyfile(run / "sd.img", webroot / "crossink-sd.img")
    class Handler(SimpleHTTPRequestHandler):
        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def log_message(self, fmt, *values):
            with (run / "http.log").open("a") as stream:
                stream.write(fmt % values + "\n")

    server = ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(webroot)))
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}/?flash=crossink-flash.bin&sd=crossink-sd.img&turbo=0&wifi=off"
    profile = run / "browser-profile"
    argv = [args.chrome, f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
            "--disable-background-networking", url]
    if args.cdp_port is not None:
        argv += ["--remote-debugging-address=127.0.0.1", f"--remote-debugging-port={args.cdp_port}"]
    details = dict(url=url, argv=argv, profile=str(profile), pid=None,
        archive_sha256=sha(archive), persistence="SD only; download SD before closing. Flash is ephemeral.")
    write_json(run / "browser.json", details)
    process = None
    try:
        with (run / "browser.log").open("wb") as log:
            process = subprocess.Popen(argv, stdout=log, stderr=log, start_new_session=True)
            details["pid"] = process.pid
            write_json(run / "browser.json", details)
            print(url, flush=True)
            end = time.monotonic() + args.overall_timeout
            while time.monotonic() < end:
                if process.poll() is not None:
                    if process.returncode:
                        raise RuntimeError(f"Browser exited with {process.returncode}")
                    return
                time.sleep(0.2)
    finally:
        ignore_termination_signals()
        try:
            stop_owned_group(process, grace=5)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


def main():
    install_signal_handlers()
    defaults = installed_defaults()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("manual", "test"))
    parser.add_argument("--device", choices=("x4-pro", "x3", "x4"), default="x4-pro")
    parser.add_argument("--release-runtime", action="store_true", help="Use the pinned native release (0.8.1 with --network, otherwise 0.5.0)")
    parser.add_argument("--network", action="store_true", help="Enable native emulated Wi-Fi through host NAT")
    parser.add_argument("--http-port", type=int, help="Forward localhost port to device HTTP (requires --network)")
    parser.add_argument("--headless", action="store_true", help="Native diagnostic run without a window")
    parser.add_argument("--backend", choices=("native", "browser"), default="native")
    parser.add_argument("--runtime", help="Pinned qemu directory; defaults to installed versioned cache")
    parser.add_argument("--build-dir", default=defaults["build_dir"], type=Path, help="CrossInk build inputs; defaults to installed verified image")
    parser.add_argument("--fixture", choices=("blank-home", "reading", "sd-assets"), default="sd-assets")
    parser.add_argument("--sd", type=Path, help="Read-only input seed; runner always copies it")
    parser.add_argument("--output", default=defaults["output"])
    parser.add_argument("--ready-timeout", type=float, default=90)
    parser.add_argument("--action-timeout", type=float, default=30)
    parser.add_argument("--overall-timeout", type=float)
    parser.add_argument("--web-archive", default=defaults["web_archive"], help="Pinned browser ZIP; defaults to installed versioned cache")
    parser.add_argument("--chrome", default="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
    parser.add_argument("--cdp-port", type=int, help="Optional private loopback CDP port, choose per run")
    args = parser.parse_args()
    profile = None
    if args.release_runtime or args.network:
        from launch import device_profile, runtime_path
        profile = device_profile(args.device, args.network)
        if args.runtime is None:
            args.runtime = str(runtime_path(args.device, args.network))
    elif args.device != "x4-pro":
        parser.error("X3/X4 require --release-runtime")
    if (args.device != "x4-pro" or args.release_runtime) and (args.mode != "manual" or args.backend != "native"):
        parser.error("Release runtime supports native manual runs; legacy acceptance/browser workflows retain their original pin")
    network_option(args.network, args.http_port)
    if args.network and (args.backend != "native" or args.mode != "manual"):
        parser.error("Networking requires a native manual run")
    if args.runtime is None:
        args.runtime = defaults["runtime"]
    if args.build_dir is None:
        parser.error("Pass --build-dir or configure build_dir in the pinned cache defaults.json")
    if args.overall_timeout is None:
        args.overall_timeout = 600 if args.mode == "test" else 3600
    if any(not math.isfinite(value) or value <= 0 for value in
           (args.ready_timeout, args.action_timeout, args.overall_timeout)):
        parser.error("Timeouts must be positive")
    if args.cdp_port is not None and not (0 <= args.cdp_port <= 65535):
        parser.error("--cdp-port must be 0 (automatic) or a valid dedicated port")
    if args.backend == "native" and not args.runtime:
        parser.error("--runtime is required for native")
    if args.backend == "browser" and (not args.web_archive or args.mode != "manual"):
        parser.error("Browser currently supports manual runs with --web-archive; native runs assertions")
    if args.mode == "test" and args.fixture != "sd-assets":
        parser.error("The five acceptance scenarios require the named sd-assets fixture")
    run = manifest = native = None
    exit_code = 1
    try:
        run, manifest = prepare(args)
        if args.backend == "browser":
            browser(args, run)
        else:
            require_debug_image(manifest["firmware"])
            native = Native(args.runtime, run, args.overall_timeout, args.action_timeout, args.mode == "manual" and not args.headless, profile=profile, network=args.network, http_port=args.http_port)
            if profile:
                manifest.update(pin=profile, timing=f"model shift={profile['shift']},sleep=on")
            native.start()
            native.ready(timeout=args.ready_timeout)
            if args.mode == "test":
                from scenarios import acceptance
                acceptance(native, manifest)
            else:
                print("Private QMP details: " + str(run / "control.json") + "; Ctrl-C stops this run.", flush=True)
                while time.monotonic() < native.budget.end:
                    if native.proc.poll() == 0:
                        break
                    native.health()
                    time.sleep(0.2)
        manifest["status"] = "passed" if args.mode == "test" else "stopped"
        exit_code = 0
    except LogProfileError as error:
        if manifest:
            manifest.update(status="unsupported-image", error=str(error), firmware_started=False)
        print(f"UNSUPPORTED IMAGE: {error}", file=sys.stderr)
        exit_code = 2
    except Terminated as error:
        if manifest:
            manifest.update(status="interrupted", signal=error.signum)
        exit_code = 128 + error.signum
    except KeyboardInterrupt:
        if manifest:
            manifest["status"] = "interrupted"
        exit_code = 0 if args.mode == "manual" else 130
    except Exception as error:
        if manifest:
            manifest.update(status="failed", error=f"{type(error).__name__}: {error}")
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        ignore_termination_signals()
        if native:
            try:
                if native.qmp and native.proc.poll() is None:
                    native.qmp.budget = Budget(5)
                    native.shot("final")
            except Exception:
                pass
            try:
                native.close()
            except Exception as error:
                if manifest:
                    manifest.update(status="failed", teardown_error=str(error))
                exit_code = 1
        if run and manifest:
            if args.backend == "native":
                exported = run / "sd-results"
                exported.mkdir(exist_ok=True)
                try:
                    extracted = subprocess.run(["mcopy", "-s", "-i", str(run / "sd.img") + "@@1048576",
                        "::/.crosspoint", str(exported)], capture_output=True, timeout=15)
                    (run / "sd-export.log").write_bytes(extracted.stdout + extracted.stderr)
                    manifest["sd_export_status"] = extracted.returncode
                except (OSError, subprocess.TimeoutExpired) as error:
                    manifest["sd_export_status"] = str(error)
            seed = Path(manifest["fixture"]["path"])
            manifest["seed_unchanged"] = sha(seed) == manifest["fixture"]["sha256"]
            if not manifest["seed_unchanged"]:
                manifest.update(status="failed", error="Input SD seed was modified")
                exit_code = 1
            if args.mode == "test":
                names = ["home-readiness", "epub-open-page-turn", "settings-change",
                    "language-hyphenation-install-from-sd", "restart-persistence"]
                completed = {s["name"] for s in manifest["scenarios"]}
                manifest["not_passed"] = [name for name in names if name not in completed]
            manifest["finished"] = time.time()
            manifest["results"] = {p.name: dict(size=p.stat().st_size, sha256=sha(p))
                for p in [run / "flash.bin", run / "sd.img", run / "serial.log", run / "stderr.log"] if p.exists()}
            write_json(run / "manifest.json", manifest)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
