#!/usr/bin/env python3
"""Exercise the compiled simulator's production stats serializer and HTTP client.

Default: local fake server, no account needed. An explicit --server plus
--credentials-file runs zero-valued uploads as crossink-simulator, then reads
them back. Credentials JSON contains username/password and is never printed.
"""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = "01b729228ded144417191bebc5a17445"


def run(program, server, credentials, error=None, empty_library=False, step=None, batch=None):
    with tempfile.TemporaryDirectory(prefix="crossink-stats-smoke-") as directory:
        (Path(directory) / "fs_").mkdir()
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", CROSSINK_SIMULATOR_SMOKE_TEST="1",
                   CROSSINK_STATS_TEST_SERVER=server,
                   CROSSINK_STATS_TEST_USER=credentials["username"],
                   CROSSINK_STATS_TEST_PASSWORD=credentials["password"])
        env.pop("CROSSINK_STATS_TEST_EMPTY_LIBRARY", None)
        env.pop("CROSSINK_STATS_TEST_BATCH", None)
        if batch is not None:
            env["CROSSINK_STATS_TEST_BATCH"] = str(batch)
        if empty_library:
            env["CROSSINK_STATS_TEST_EMPTY_LIBRARY"] = "1"
        if step is not None:
            env["CROSSINK_STATS_TEST_STEP"] = step
        else:
            env.pop("CROSSINK_STATS_TEST_STEP", None)
        if error is not None:
            env["CROSSINK_STATS_TEST_ERROR"] = str(error)
        else:
            env.pop("CROSSINK_STATS_TEST_ERROR", None)
        result = subprocess.run([str(program)], cwd=directory, env=env, capture_output=True, text=True, timeout=120)
        marker = "All-time stats failed-step smoke passed" if step else "Stats upload transport smoke passed"
        if result.returncode or marker not in result.stdout + result.stderr:
            # Log only diagnostics; never dump request headers or credentials.
            lines = [line for line in (result.stdout + result.stderr).splitlines() if "StatsSync" in line or "SMOKE" in line]
            raise RuntimeError(f"Simulator upload failed (exit {result.returncode}):\n" + "\n".join(lines) + result.stderr[-1000:])

        log_path = Path(directory) / "fs_/.crossink/sync.log"
        if not log_path.is_file():
            raise RuntimeError("Sync diagnostic log was not exported to /.crossink/sync.log")
        log = log_path.read_text()
        if not all(field in log for field in ("utc=", "uptime_ms=", "http=", "result=")):
            raise RuntimeError("Sync log lacks request diagnostics")
        for secret in (credentials["username"], credentials["password"],
                       hashlib.md5(credentials["password"].encode()).hexdigest()):
            if secret and secret in log:
                raise RuntimeError("Sync log contains credentials")


def local_tests(program):
    requests = []
    mode = "ok"

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            status = 404 if mode == "old-streak-404" else 405 if mode == "old-streak-405" else 200
            reply = {"v": 1, "anchor_day": 9769, "streak": 1,
                     "history_b64": base64.b64encode(bytes([1]) + bytes(91)).decode()}
            if mode in ("streak", "empty-streak"):
                reply = {"v": 1}  # HTTP success, invalid calendar.
            data = json.dumps(reply).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_PUT(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert self.headers["x-auth-user"] == "smoke"
            assert self.headers["x-auth-key"] == hashlib.md5(b"smoke-password").hexdigest()
            assert "Authorization" not in self.headers
            requests.append((self.path, body))
            status = {"auth": 401, "unsupported": 404, "server": 500}.get(mode, 200)
            if mode == "totals":
                status = 500
            if mode.startswith("missing-counters-") and self.path.startswith("/api/v1/stats/counters/"):
                status = int(mode.rsplit("-", 1)[1])
            reply = {"until": 1, "accepted": 1, "accepted_daily": 1, "counter_ids": 1}
            if self.path == "/api/v1/stats/books":
                reply["accepted"] = len(body["items"])
                if mode == "partial-batch":
                    reply["accepted"] -= 1
                elif mode == "extra-batch":
                    reply["accepted"] += 1
                elif mode == "missing-batch":
                    reply.pop("accepted")
            if "daily" in body and mode == "daily":
                reply.pop("accepted_daily")
            if self.path == "/api/v1/stats/sessions" and mode != "sessions":
                reply["accepted_session_ids"] = [body["items"][0]["session_id"]]
            if mode == "invalid":
                reply = {"until": "not a timestamp"}
            elif mode == "oversize":
                reply = {"until": 1, "padding": "x" * 512}
            data = json.dumps(reply).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}"
    try:
        run(program, url, {"username": "smoke", "password": "smoke-password"})
        assert len(requests) == 4
        assert requests[0] == requests[1] and requests[2] == requests[3]
        assert requests[0][0] == "/api/v1/stats/global"
        assert requests[2][0] == "/api/v1/stats/books"
        assert len(base64.b64decode(requests[0][1]["history_b64"])) == 92
        assert requests[2][1]["items"][0]["document"] == DOCUMENT
        print("PASS: production payloads, headers, global/book uploads, identical retries")
        for mode, error in [("ok", None), ("partial-batch", 4), ("extra-batch", 4), ("missing-batch", 4)]:
            for count in (1, 2, 10):
                run(program, url, {"username": "smoke", "password": "smoke-password"}, error, batch=count)
                assert len(requests[-1][1]["items"]) == count
            print(f"PASS: {mode} acknowledgment for batches of 1, 2 and 10 books")
        for mode, error in [("auth", 2), ("unsupported", 3), ("invalid", 4), ("oversize", 4), ("server", 6)]:
            run(program, url, {"username": "smoke", "password": "smoke-password"}, error)
            print(f"PASS: {mode} response")
        mode = "ok"
        requests.clear()
        run(program, url, {"username": "smoke", "password": "smoke-password"}, empty_library=True)
        assert len(requests) == 1 and requests[0][0] == "/api/v1/stats/counters/global"
        print("PASS: confirmed activity uploads overall stats with an empty Library")
        for mode in ("totals", "daily", "sessions", "corrupt-session", "streak", "empty-streak",
                     "old-streak-404", "old-streak-405", "missing-counters-404", "missing-counters-405", "ok"):
            run(program, url, {"username": "smoke", "password": "smoke-password"}, step=mode)
            print(f"PASS: all-time failed-step attribution: {mode}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def read_back(server, path, credentials):
    # curl uses the host's certificate store; pass auth through stdin, not argv.
    key = hashlib.md5(credentials["password"].encode()).hexdigest()
    username = credentials["username"]
    if any(c in username for c in '\r\n"\\'):
        raise ValueError("Unsupported username for curl config")
    config = f'header = "x-auth-user: {username}"\nheader = "x-auth-key: {key}"\n'
    result = subprocess.run(["curl", "--fail", "--silent", "--show-error", "--max-time", "20",
                             "--config", "-", server.rstrip("/") + path],
                            input=config, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, default=ROOT / ".pio/build/simulator/program")
    parser.add_argument("--server")
    parser.add_argument("--credentials-file", type=Path)
    args = parser.parse_args()
    if bool(args.server) != bool(args.credentials_file):
        parser.error("--server and --credentials-file must be supplied together")
    if not args.server:
        local_tests(args.program.resolve())
        return
    credentials = json.loads(args.credentials_file.read_text())
    # Refuse to replace any existing simulator history on a live account.
    before = read_back(args.server, "/api/v1/stats/global", credentials)
    if any(d["device_id"] == "crossink-simulator" for d in before["devices"]):
        raise RuntimeError("Account already has a simulator snapshot; refusing to replace it")
    run(args.program.resolve(), args.server, credentials)
    after = read_back(args.server, "/api/v1/stats/global", credentials)
    device = next(d for d in after["devices"] if d["device_id"] == "crossink-simulator")
    assert device["stats"]["seconds"] == 0 and device["stats"]["sessions"] == 0
    books = read_back(args.server, f"/api/v1/stats/books/{DOCUMENT}", credentials)
    device_book = next(d for d in books["devices"] if d["device_id"] == "crossink-simulator")
    assert device_book["stats"]["seconds"] == 0
    print("PASS: live global/book uploads, retries, and authenticated read-back (zero-valued simulator snapshots)")


if __name__ == "__main__":
    main()
