#!/usr/bin/env python3
"""Exercise SD plugins against isolated HTTP/TLS fixtures and a simulator build."""
import argparse
import gzip
import base64
import hashlib
import zipfile
import zlib
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env', default='x4-pro-simulator')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='crossink-plugin-smoke-') as directory:
        work = Path(directory)
        sd = work / 'fs_'
        def write(path, value):
            file = sd / path.lstrip('/')
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(json.dumps(value) if isinstance(value, (dict, list)) else value)
        # Real AES fixtures: raw-deflate ADEPT and stored LCP resources.
        for bits in (128, 256):
            plain = b'<html><body>Protected fixture text</body></html>'
            if bits == 128:
                compressor = zlib.compressobj(wbits=-15)
                payload = compressor.compress(plain) + compressor.flush()
            else:
                payload = plain
            key = bytes(range(bits // 8)); iv = bytes(range(16))
            cipher = subprocess.run(['openssl', 'enc', f'-aes-{bits}-cbc', '-K', key.hex(), '-iv', iv.hex()],
                                    input=payload, capture_output=True, check=True).stdout
            book = sd / f'Books/protected{bits}.epub'; book.parent.mkdir(parents=True, exist_ok=True)
            manifest = f'<encryption><EncryptedData><EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes{bits}-cbc"/><CipherData><CipherReference URI="chapter.xhtml"/></CipherData><EncryptionProperties><Compression Method="{8 if bits == 128 else 0}" OriginalLength="{len(plain)}"/></EncryptionProperties></EncryptedData></encryption>'
            if bits == 128:
                manifest = manifest.replace(f' OriginalLength="{len(plain)}"', '')
                manifest = manifest.replace('Algorithm=', 'Algorithm = ').replace('URI=', 'URI = ')
            with zipfile.ZipFile(book, 'w') as archive:
                archive.writestr('mimetype', 'application/epub+zip')
                archive.writestr('META-INF/container.xml', '<container><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
                archive.writestr('content.opf', '<package version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">protected-fixture</dc:identifier><dc:title>Protected fixture</dc:title><dc:creator>Test author</dc:creator><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>')
                archive.writestr('META-INF/encryption.xml', manifest)
                archive.writestr('chapter.xhtml', iv + cipher)
                archive.writestr('plain.txt', 'plain resource')
        tilt_preferences = {'tiltPageTurn': 1, 'tiltPageTurnDirection': 2, 'tiltNextPageOnly': 1}
        if args.env == 'simulator':
            write('/.crosspoint/crossink-settings.json', tilt_preferences)
        event_attempts = []
        encoded_downloads = set()
        class Fixture(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args):
                pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                if self.path == '/events':
                    event_attempts.append(body)
                    self.send_response(503 if len(event_attempts) == 1 else 200)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                if self.path == '/auth':
                    value = {'user_code': 'TEST-123', 'verification_uri': 'https://example.invalid/activate',
                             'device_code': 'fixture-code', 'interval': 3, 'expires_in': 60}
                else:
                    assert b'fixture-code' in body
                    value = {'access_token': 'signed-in'}
                payload = json.dumps(value).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def do_GET(self):
                url = f'https://localhost:{self.server.server_port}'
                if self.path == '/signed':
                    assert self.headers.get('Authorization') == 'Bearer signed-in'
                    data = json.dumps([{'title': 'Signed-in library', 'url': url + '/book'}]).encode()
                elif self.path == '/json':
                    # A filtered row shares the title, but has a different ID/version.
                    # Its large details must neither match nor exhaust a retained-row budget.
                    rows = [{'title': 'Fixture', 'id': 'wrong', 'version': '0',
                             'requires': 'catalog.missing', 'description': 'wrong' * 380,
                             'author': 'a' * 1900, 'repository': 'r' * 1900,
                             'details': {'url': url + '/wrong-details', 'format': 'html'}}]
                    rows += [{'title': title, 'id': str(i), 'version': '1', 'url': url + '/book/Café stories',
                              'description': 'selected' + 'x' * 1890, 'author': 'a' * 1900,
                              'repository': 'r' * 1900,
                              'details': {'url': url + '/details', 'format': 'html'}}
                             for i, title in enumerate(['Fixture', 'Second book', 'Café stories'])]
                    data = json.dumps(rows).encode()
                elif self.path == '/details':
                    data = ('<h1>Fixture details</h1><p>Café &amp; catalog</p>' + '<p>Long description for paging.</p>' * 60).encode()
                elif self.path == '/lists':
                    data = json.dumps({'lists': [{'title': 'Books', 'url': url + '/json'}]}).encode()
                elif self.path == '/xml':
                    data = ('<files>' + ''.join(f'<file url="{url}/book/{i}" title="Book {i:03}"/>'
                                                for i in range(200)) + '</files>').encode()
                elif self.path == '/bundles':
                    data = json.dumps([{'id': 'installed', 'title': 'Example plugin',
                                        'base': url + '/bundle/Café scripts', 'files': ['plugin.js']}]).encode()
                elif self.path == '/missing':
                    self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
                elif self.path in ('/book/Caf%C3%A9%20stories', '/bundle/Caf%C3%A9%20scripts/plugin.js'):
                    encoded_downloads.add(self.path)
                    data = b'fixture book contents\n'
                elif self.path == '/echo':
                    data = json.dumps({'authorization': self.headers.get('Authorization')}).encode()
                else:
                    data = b'fixture book contents\n'
                status = 200
                total = len(data)
                if self.headers.get('Range'):
                    offset = int(self.headers['Range'].split('=')[1].split('-')[0])
                    data = data[offset:]
                    status = 206
                    self.send_response(status)
                    self.send_header('Content-Range', f'bytes {offset}-{total - 1}/{total}')
                else:
                    self.send_response(status)
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('ETag', '"fixture-v1"')
                self.end_headers()
                self.wfile.write(data)
        fixture = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
        cert, key = work / 'cert.pem', work / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        fixture.socket = context.wrap_socket(fixture.socket, server_side=True)
        threading.Thread(target=fixture.serve_forever, daemon=True).start()
        origin = f'https://localhost:{fixture.server_port}'
        write('/plugins/events/device.json', {'title': 'Event fixture', 'events': {name: {'connect': name == 'reader.exit', 'request': {'url': origin + '/events', 'body': '{event.id}:{event.book}:{event.ts}'}} for name in ['reader.open', 'reader.exit', 'reader.session', 'book.downloaded', 'sleep.enter']}})
        write('/plugins/json/device.json', {'title': 'Library catalog', 'browse': {'url': origin + '/json',
              'fields': {'title': 'title', 'id': 'id', 'version': 'version', 'url': 'url',
                         'description': 'description', 'detail_author': 'author', 'repository': 'repository',
                         'details_url': 'details.url', 'details_format': 'details.format'}, 'lists_url': origin + '/lists',
              'compatibility': {'requires': 'requires'}},
              'download': {'url': '{url}', 'filename': '{title}.txt', 'dest_dir': '/Books'}})
        write('/plugins/auth/device.json', {'title': 'Sign-in catalog',
              'token': {'file': 'token.json', 'path': 'access_token'},
              'browse': {'url': origin + '/signed', 'headers': {'Authorization': 'Bearer {token}'},
                         'fields': {'title': 'title', 'url': 'url'}},
              'download': {'url': '{url}'},
              'auth': {'type': 'device_code', 'request': {'url': origin + '/auth'},
                       'poll': {'url': origin + '/token', 'body': '{"code":"{device_code}"}'}}})
        write('/plugins/json/plugin.js', 'CrossPoint.registerPlugin((container) => { container.textContent = "Fixture plugin"; });')
        write('/plugins/xml/device.json', {'title': 'XML directory', 'browse': {'url': origin + '/xml',
              'format': 'xml', 'item': 'file', 'fields': {'title': '@title', 'url': '@url'}}, 'download': {}})
        write('/plugins/bundles/device.json', {'title': 'Plugin catalog', 'browse': {'url': origin + '/bundles',
              'fields': {'title': 'title', 'id': 'id'}}, 'download': {'dest_dir': '/.crosspoint/plugins',
              'bundle': {'base': 'base', 'files': 'files', 'subdir': '{id}'}}})
        write('/plugins/installed/manifest.json', {'title': 'Browser tools'})
        write('/plugins/installed/plugin.js', 'old')
        write('/plugins/installed/.config.json', {'keep': True})
        write('/plugins/installed/.secrets/value', 'preserve')
        write('/.crosspoint/wifi.json', {'private': True})
        with socket.socket() as port_probe:
            port_probe.bind(('127.0.0.1', 0)); port = port_probe.getsockname()[1]
        env = os.environ | {'SDL_VIDEODRIVER': 'dummy', 'CROSSINK_SIMULATOR_SMOKE_TEST': '1',
            'CROSSINK_SIMULATOR_SMOKE_PLUGINS': '1', 'CROSSINK_PLUGIN_CAPTURES': str(args.output.resolve()),
            'CROSSPOINT_SIM_HTTP_PORT': str(port)}
        log = args.output / 'plugin-smoke.log'
        with log.open('w') as output:
            process = subprocess.Popen([str(ROOT / '.pio/build' / args.env / 'program')], cwd=work, env=env,
                                       stdout=output, stderr=subprocess.STDOUT)
            base = f'http://127.0.0.1:{port}'
            def request(path, body=None, expected=200, headers=None):
                data = json.dumps(body).encode() if isinstance(body, dict) else body
                hdr = {'Content-Type': 'application/json'} if isinstance(body, dict) else {}
                hdr.update(headers or {})
                req = urllib.request.Request(base + path, data=data, headers=hdr)
                try:
                    response = urllib.request.urlopen(req, timeout=20)
                except urllib.error.HTTPError as error:
                    response = error
                assert response.status == expected, (path, response.status, response.read())
                result = response.read()
                if response.headers.get('Content-Encoding') == 'gzip': result = gzip.decompress(result)
                return result, response.headers
            try:
                deadline = time.monotonic() + 90
                while True:
                    if process.poll() is not None: raise RuntimeError(log.read_text()[-5000:])
                    try:
                        plugins, _ = request('/api/plugins')
                        break
                    except (urllib.error.URLError, ConnectionError):
                        if time.monotonic() >= deadline: raise TimeoutError(log.read_text()[-5000:])
                        time.sleep(.1)
                if args.env == 'simulator':
                    settings_body, _ = request('/api/settings')
                    settings = {row['key']: row for row in json.loads(settings_body)}
                    assert not tilt_preferences.keys() & settings.keys(), settings.keys()
                    for key in ('shortPwrBtn', 'longPwrBtn', 'longPressMenuAction', 'longPressBackAction'):
                        assert not any('tilt' in option.lower() for option in settings[key]['options'])
                    last_action = len(settings['shortPwrBtn']['options']) - 1
                    request('/api/settings', {'shortPwrBtn': last_action} | dict.fromkeys(tilt_preferences, 0))
                    settings_body, _ = request('/api/settings')
                    settings = {row['key']: row for row in json.loads(settings_body)}
                    assert settings['shortPwrBtn']['value'] == last_action
                    saved = json.loads((sd / '.crosspoint/crossink-settings.json').read_text())
                    assert all(saved[key] == value for key, value in tilt_preferences.items()), saved
                assert len(event_attempts) >= 6 and event_attempts[0] == event_attempts[1], event_attempts
                assert any(p['name'] == 'json' for p in json.loads(plugins))
                script, script_headers = request('/plugin?name=json&file=plugin.js')
                assert b'registerPlugin' in script
                etag = script_headers['ETag']
                assert etag == '"' + hashlib.md5(script).hexdigest() + '"'
                assert script_headers['Cache-Control'] == 'no-cache'
                unchanged, cached_headers = request('/plugin?name=json&file=plugin.js', expected=304,
                                                    headers={'If-None-Match': etag})
                assert unchanged == b'' and cached_headers['ETag'] == etag
                # Same-size edits must invalidate the validator even without a clock.
                updated = script.replace(b'registerPlugin', b'registerPlugiN', 1)
                write('/plugins/json/plugin.js', updated.decode())
                changed, changed_headers = request('/plugin?name=json&file=plugin.js',
                                                   headers={'If-None-Match': etag})
                assert changed == updated and changed_headers['ETag'] != etag
                write('/plugins/json/plugin.js', script.decode())
                host, _ = request('/plugin-host.js'); assert b'registerPlugin' in host
                runner, _ = request('/plugins-run'); assert b'plugin-host.js' in runner
                relay, headers = request('/api/relay', {'plugin': 'json', 'url': origin + '/echo',
                                         'headers': {'Authorization': 'Bearer fixture'}})
                assert headers['X-Relay-Status'] == '200'
                assert json.loads(relay)['authorization'] == 'Bearer fixture'
                result, _ = request('/api/crypto', {'op': 'sha256', 'data': base64.b64encode(b'fixture').decode()})
                assert base64.b64decode(json.loads(result)['data']) == hashlib.sha256(b'fixture').digest()
                a, _ = request('/api/status?plugin=json'); b, _ = request('/api/status?plugin=other')
                assert len(json.loads(a)['deviceId']) == 64 and json.loads(a)['deviceId'] != json.loads(b)['deviceId']
                request('/api/book-key', {}, expected=400)
                request('/api/book-key', {'path': '/Books/protected256.epub', 'key': base64.b64encode(bytes(range(32))).decode(), 'expires': 0})
                assert len((sd / 'Books/protected256.epub.key').read_bytes()) == 74

                request('/api/fetch', {'url': origin + '/book', 'dest': '/.crosspoint/wifi.json'}, expected=400)
                request('/download?path=/.crosspoint/wifi.json', expected=403)
                write('/Books/browser.txt', 'old book')
                offset = 0
                for _ in range(20):
                    result, _ = request('/api/fetch', {'url': origin + '/book', 'dest': '/Books/browser.txt',
                                                      'maxBytes': 5, 'offset': offset})
                    result = json.loads(result)
                    assert not result.get('error'), result
                    if result['complete']: break
                    assert (sd / 'Books/browser.txt').read_text() == 'old book'
                    offset = result['bytes']
                else: raise AssertionError('fetch never completed')
                assert (sd / 'Books/browser.txt').read_bytes() == b'fixture book contents\n'
                result, _ = request('/api/fetch', {'url': origin + '/missing', 'dest': '/Books/browser.txt'})
                assert json.loads(result).get('error') or not json.loads(result)['complete']
                assert (sd / 'Books/browser.txt').read_bytes() == b'fixture book contents\n'
                boundary = 'plugin-test-boundary'
                multipart = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="file"\r\n'
                             'Content-Type: application/octet-stream\r\n\r\nupdated\r\n'
                             f'--{boundary}--\r\n').encode()
                request('/api/plugin-fs?plugin=json&path=/plugins/json/config.json', multipart,
                        headers={'Content-Type': f'multipart/form-data; boundary={boundary}'})
                assert (sd / 'plugins/json/config.json').read_text() == 'updated'
                submitted, _ = request('/api/plugin-jobs', {'plugin': 'json', 'action': 'refresh', 'args': {'a': 1}})
                job = json.loads(submitted)['id']
                claimed, _ = request('/api/plugin-jobs/claim?plugin=json'); claim = json.loads(claimed)['claim']
                request('/api/plugin-jobs/complete', {'id': job, 'claim': claim + 1, 'ok': True}, expected=409)
                request('/api/plugin-jobs/complete', {'id': job, 'claim': claim, 'ok': True, 'result': {'done': True}})
                status, _ = request(f'/api/plugin-jobs/status?id={job}'); assert json.loads(status)['state'] == 'done'
                write('/plugin-web-done', 'done')
                assert process.wait(timeout=10) == 0
                assert encoded_downloads == {
                    "/book/Caf%C3%A9%20stories", "/bundle/Caf%C3%A9%20scripts/plugin.js"
                }, encoded_downloads
                other_env = env | {'CROSSINK_VERIFY_COPIED_KEYS': '1',
                                   'CROSSINK_DEVICE_SECRET_PATH': str(work / 'second-device-secret')}
                copied = subprocess.run([str(ROOT / '.pio/build' / args.env / 'program')], cwd=work,
                                        env=other_env, stdout=output, stderr=subprocess.STDOUT, timeout=30)
                assert copied.returncode == 0, 'Copied key accepted or second-device test failed'

            finally:
                if process.poll() is None: process.kill(); process.wait()
                fixture.shutdown()
        print('Plugin smoke passed: catalogs, touch, HTTPS, repeated bundles, sign-in, tokens, relay, staged fetch, uploads, jobs, protected paths, AES-128/AES-256, wrapped keys, expiry, tamper and second-device rejection.')
        print(f'Screenshots and log: {args.output.resolve()}')


if __name__ == '__main__':
    main()
