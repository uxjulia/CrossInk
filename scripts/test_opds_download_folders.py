#!/usr/bin/env python3
"""Exercise the real simulator OPDS/settings APIs with an isolated SD card."""
from __future__ import annotations

import argparse
import gzip
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def run(program: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="crossink-opds-folders-") as directory:
        root = Path(directory)
        sd = root / "fs_"
        config = sd / ".crosspoint"
        config.mkdir(parents=True)
        store = config / "opds.json"
        store.write_text(json.dumps({"servers": [{"name": "Old", "url": "https://old.example/opds"}]}))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_SD=str(sd), CROSSPOINT_SIM_HTTP_PORT=str(port),
                   CROSSPOINT_SIM_SILENT_REBOOT_MAGIC=str(0xC1EAB007), CROSSPOINT_SIM_SILENT_REBOOT_TARGET="6",
                   CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD="2")
        env.pop("CROSSINK_SIMULATOR_SMOKE_TEST", None)
        base = f"http://127.0.0.1:{port}"
        with (root / "simulator.log").open("w") as log:
            process = subprocess.Popen([str(program)], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                def request(path, body=None):
                    data = None if body is None else json.dumps(body).encode()
                    req = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"})
                    try:
                        response = urllib.request.urlopen(req, timeout=10)
                    except urllib.error.HTTPError as error:
                        response = error
                    with response:
                        body = response.read()
                        if response.headers.get("Content-Encoding") == "gzip":
                            body = gzip.decompress(body)
                        return response.status, body.decode()

                deadline = time.monotonic() + 45
                while True:
                    try:
                        status, _ = request("/")
                        if status == 200:
                            break
                    except OSError:
                        pass
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Simulator web portal failed to start")
                    time.sleep(0.1)

                def servers():
                    status, body = request("/api/opds")
                    assert status == 200, body
                    return json.loads(body)

                def post(body, status=200):
                    actual, message = request("/api/opds", body)
                    assert actual == status, (actual, message, body)

                assert servers()[0]["downloadFolder"] == ""
                post({"index": 0, "name": "Calibre", "url": "https://calibre.example/opds", "password": "secret",
                      "filenameFormat": "server_filename", "downloadFolder": " Books//Calibre/ "})
                assert servers()[0]["downloadFolder"] == "/Books/Calibre"
                # Older clients omit the override and must preserve it and credentials.
                post({"index": 0, "name": "Older client", "url": "https://calibre.example/opds"})
                saved = servers()[0]
                assert saved["downloadFolder"] == "/Books/Calibre" and saved["hasPassword"]
                assert saved["filenameFormat"] == "server_filename" and "password" not in saved
                post({"name": "Root", "url": "https://root.example", "downloadFolder": "/"})
                assert servers()[1]["downloadFolder"] == "/"
                before = store.read_bytes()
                for folder in [None, 42, [], "/Books/../Private", "/Books/a:b", "/a\x00hidden", "/" + "é" * 32]:
                    post({"index": 0, "downloadFolder": folder}, 400)
                    assert store.read_bytes() == before
                status, body = request("/api/settings", {"opdsDownloadFolder": " Incoming//Books/ "})
                assert status == 200, body
                settings = json.loads(request("/api/settings")[1])
                assert next(s["value"] for s in settings if s["key"] == "opdsDownloadFolder") == "/Incoming/Books"
                assert servers()[0]["downloadFolder"] == "/Books/Calibre"
                assert request("/api/settings", {"opdsDownloadFolder": "../Invalid"})[0] == 400
                post({"index": 0, "name": "Calibre", "downloadFolder": ""})
                assert servers()[0]["downloadFolder"] == ""
                # Escaped, long JSON entries must not be truncated or skipped.
                post({"name": 'Quoted "catalog" ' * 35, "url": "https://long.example/" + "a" * 600,
                      "downloadFolder": "/" + "b" * 62})
                assert len(servers()) == 3 and len(servers()[2]["downloadFolder"].encode()) == 63
                # Force a disk failure and verify the API reports it, preserving RAM state.
                snapshot = store.read_bytes()
                store.chmod(0o444)
                try:
                    post({"index": 0, "name": "Failed update", "downloadFolder": "/Failed"}, 500)
                    assert servers()[0]["downloadFolder"] == ""
                    assert store.read_bytes() == snapshot
                finally:
                    store.chmod(0o644)
                print("PASS: real OPDS API inheritance/root/custom, old clients, validation, streamed JSON, save failure")
                print("PASS: shared default normalization and independent server overrides")
            except BaseException:
                log.flush()
                print((root / "simulator.log").read_text()[-8000:])
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", default="simulator")
    args = parser.parse_args()
    run(ROOT / ".pio/build" / args.env / "program")
