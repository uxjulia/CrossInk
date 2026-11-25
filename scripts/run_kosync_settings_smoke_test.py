#!/usr/bin/env python3
"""Validate sync profiles, credential migration and proxy headers against a local server."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[1]
requests = []
errors = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, body):
        if self.headers.get('X-Proxy-Token') != 'fixture-secret':
            errors.append('Missing proxy token: ' + self.path)
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == '/users/auth':
            self.reply({'authorized': 'OK'})
        elif url.path.endswith('/percentage-fixture'):
            if parse_qs(url.query).get('position_kinds') != ['locator,percentage']:
                errors.append('Missing position capabilities')
            self.reply({'progress': None, 'percentage': 0.625, 'position_kind': 'percentage', 'device': 'Web'})
        else:
            self.reply({'position_kinds_available': ['percentage']})

    def do_PUT(self):
        requests.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
        self.reply({})

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='kosync-settings-') as directory:
        (Path(directory) / 'fs_/.crosspoint').mkdir(parents=True)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', CROSSINK_SIMULATOR_SMOKE_TEST='1',
                   CROSSINK_KOSYNC_SETTINGS_TEST='1',
                   CROSSINK_READING_TEST_SERVER=f'http://127.0.0.1:{server.server_port}')
        result = subprocess.run([str(ROOT / '.pio/build/simulator/program')], cwd=directory,
                                env=env, capture_output=True, text=True, timeout=90)
        log = result.stdout + result.stderr
        if result.returncode or 'KOReader server settings passed' not in log:
            raise AssertionError(log[-12000:])
    assert not errors, errors
    assert len(requests) == 3, requests
    strict, crosspoint, other = requests
    assert 'position' not in strict and 'isbn' not in strict['metadata']
    assert 'position' in crosspoint and 'isbn' not in crosspoint['metadata']
    assert 'position' in other and other['metadata']['isbn'] == '9781234567890'
    assert other['metadata']['series_index'] == 2.5
    print('PASS: migrations, obfuscation, header validation, proxy auth, percentage-only GET, profile payloads')
finally:
    server.shutdown()
