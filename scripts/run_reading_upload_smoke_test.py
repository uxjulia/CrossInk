#!/usr/bin/env python3
"""Exercise confirmed recursive uploads against an isolated local server and SD fixture."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]


def read_sync_ids(fs):
    """Parses /.crosspoint/sync_ids.bin (docs/file-formats.md), checking every CRC."""
    import zlib
    data = (fs / '.crosspoint/sync_ids.bin').read_bytes()
    assert data[0] == 1, 'sync_ids version'
    assert int.from_bytes(data[6:10], 'little') == zlib.crc32(data[:6]), 'sync_ids header crc'
    out, offset = {}, 10
    while offset < len(data):
        entry_id, length = data[offset:offset + 16], data[offset + 16]
        path = data[offset + 17:offset + 17 + length]
        crc = int.from_bytes(data[offset + 17 + length:offset + 21 + length], 'little')
        assert crc == zlib.crc32(data[offset:offset + 17 + length]), ('sync_ids entry crc', path)
        out[path.decode()] = entry_id.hex()
        offset += 21 + length
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, default=ROOT / '.pio/build/simulator/program')
    parser.add_argument('--mode', action='append', help='Run only the named scenarios (repeatable)')
    parser.add_argument('--touch', action='store_true', help='Include the touch Back check (requires a touch simulator)')
    args = parser.parse_args()
    requests = []
    second = []  # requests after the harness starts a second sync
    gets = []
    snapshot = {}
    probes = []
    mode = 'ok'
    failed_document = None

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            reply = {}
            (second if (fs / 'second-sync-started').exists() else gets).append(('GET', self.path))
            if self.path.startswith('/api/v1/clippings/'):
                from urllib.parse import parse_qs, urlparse
                url = urlparse(self.path)
                document = url.path.rsplit('/', 1)[-1]
                query = parse_qs(url.query)
                cursor = int(query['cursor'][0])
                items = []
                if mode in ('clippings-download', 'folder-download') and document == hashlib.md5(b'unread.epub').hexdigest() and cursor == 0:
                    items = [{'id': 'other-reader-highlight', 'revision': 1, 'deleted': 0,
                              'spine': 0, 'start_page': 0, 'end_page': 0, 'pages': 1,
                              'start_word': 0, 'end_word': 3, 'words': 4, 'para': None,
                              'chapter': 'Chapter', 'text': 'Selection from another reader',
                              'created_at': 1790000000, 'layout_signature': 123}]
                data = json.dumps({'document': document, 'sync_version': 2,
                                   'cursor': 1 if items else cursor, 'more': False, 'items': items}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if self.path.startswith('/api/v1/progress/changes') and mode.startswith('feed-'):
                from urllib.parse import parse_qs, urlparse
                query = parse_qs(urlparse(self.path).query)
                assert query['device'] == ['crossink-simulator'], self.path
                since = int(query['since'][0])
                status, reply = 200, {'changes': [], 'cursor': 5 if since == 0 else since, 'more': False}
                if mode == 'feed-404':
                    status, reply = 404, {}
                elif since == 5:
                    cache = fs / (fs / 'first-cache.txt').read_text().lstrip('/')
                    snapshot['first'] = (cache / 'progress.bin').read_bytes()
                    # first.epub moved ahead on another reader, plus a book this
                    # reader does not have (parked for when it is opened).
                    remote = json.loads((fs / 'remote-progress.json').read_text())
                    remote['document'] = read_sync_ids(fs)['/read/first.epub']
                    remote['aliases'] = []
                    unknown = dict(remote, document='ab' * 16, device='Phone')
                    reply = {'changes': [remote, unknown], 'cursor': 7, 'more': False}
                data = json.dumps(reply).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if self.path == '/api/v1/stats/summary':
                probes.append(self.path)
                # Only Authenticate probes; a sync must never ask.
                status = 200
                # A catch-all proxy page must not count as the stats API.
                data = json.dumps({'devices': []}).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if mode in ('current-equal', 'current-ask-equal', 'current-download', 'current-menu-cold', 'folder-mixed', 'folder-equal', 'folder-ask', 'folder-ask-cancel', 'folder-ask-skip', 'folder-ask-skip-landscape', 'folder-ask-exit-held', 'folder-local-ahead'):
                expected = json.loads((fs / 'expected-progress.json').read_text())
                if mode == 'current-menu-cold':
                    expected['document'] = hashlib.md5(b'unread.epub').hexdigest()
                if self.path.split('?', 1)[0].endswith(expected['document']):
                    reply = expected
                    if mode == 'folder-local-ahead':
                        reply['percentage'] *= 0.1
                    if mode in ('current-download', 'folder-mixed', 'folder-ask', 'folder-ask-cancel', 'folder-ask-skip',
                                'folder-ask-skip-landscape', 'folder-ask-exit-held'):
                        reply = json.loads((fs / 'remote-progress.json').read_text())
            data = json.dumps(reply).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_PUT(self):
            nonlocal failed_document
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.headers['x-auth-user'] == 'smoke'
            assert self.headers['x-auth-key'] == hashlib.md5(b'smoke-password').hexdigest()
            (second if (fs / 'second-sync-started').exists() else requests).append((self.path, body))
            status = 200
            reply = {'until': 1, 'accepted': 1, 'timestamp': 1, 'document': body.get('document', '')}
            if mode == 'auth':
                status = 401
            if mode == 'unsupported' and self.path.startswith('/api/v1/'):
                status = 404
            if mode == 'folder-global-failure' and self.path == '/api/v1/stats/global':
                status = 500
            if mode == 'feed-batch-404' and self.path == '/api/v1/progress/batch':
                status = 404  # a server with the change feed but no batch route
            if mode == 'feed-batch-500' and self.path == '/api/v1/progress/batch':
                status = 500
            failure_endpoint = ('/api/v1/stats/books' if mode == 'folder-skip-stats' else
                                '/api/v1/clippings/' if mode == 'folder-skip-clippings' else '/syncs/progress')
            if mode.startswith('folder-skip-') and 'invalid' not in mode and self.path.startswith(failure_endpoint):
                document = body.get('document') or (body.get('items') or [{}])[0].get('document') or self.path.rsplit('/', 1)[-1]
                if failed_document is None:
                    failed_document = document
                if document == failed_document or mode == 'folder-skip-all':
                    status = 500
            data = json.dumps(reply).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        modes = ['clippings-download', 'folder-download', 'ok', 'folder-xtc', 'all-books', 'stats-entry', 'stats-twice', 'clippings-only', 'clippings-twice', 'xtc-book', 'disabled', 'cancel', 'auth', 'unsupported', 'folder-mixed', 'folder-equal', 'folder-local-ahead', 'folder-ask', 'folder-ask-cancel', 'folder-ask-skip', 'folder-ask-skip-landscape', 'folder-ask-exit-held', 'folder-exit-held', 'folder-missing-cache', 'folder-skip-progress', 'folder-skip-stats', 'folder-skip-clippings', 'folder-skip-invalid', 'folder-skip-all', 'current-upload', 'current-upload-crosspoint', 'current-equal', 'current-ask-upload', 'current-ask-equal', 'current-download', 'current-menu', 'current-menu-cold', 'folder-no-progress', 'folder-legacy-progress', 'folder-zero-count', 'folder-past-end', 'folder-empty', 'folder-global-failure', 'folder-no-stats', 'folder-tracking-disabled', 'feed-twice', 'feed-parked', 'feed-404', 'feed-batch-404', 'feed-batch-500', 'current-menu-kept', 'open-kept-accept', 'open-kept-decline', 'open-kept-behind', 'open-kept-unprepared', 'matching-confirm', 'matching-cancel', 'matching-web']
        if args.touch:
            modes += ['folder-done-touch']
        modes += ['folder-done-confirm']
        if args.mode:
            unknown = set(args.mode) - set(modes)
            if unknown:
                parser.error(f"Unknown scenarios: {', '.join(sorted(unknown))}")
            modes = args.mode
        for mode in modes:
            requests.clear()
            second.clear()
            gets.clear()
            probes.clear()
            failed_document = None
            with tempfile.TemporaryDirectory(prefix='crossink-reading-upload-') as directory:
                fs = Path(directory) / 'fs_'
                for folder in ('read/sub', 'read/.hidden', 'read-sibling'):
                    (fs / folder).mkdir(parents=True)
                fixture = ROOT / 'test/epubs/test_reader_rendering_matrix.epub'
                for book in ('read/first.epub', 'read/sub/second.EPUB', 'read/unread.epub',
                             'read/.hidden/hidden.epub', 'read-sibling/outside.epub'):
                    shutil.copy2(fixture, fs / book)
                (fs / 'read/notes.txt').write_text('Not an EPUB')
                (fs / 'My Clippings.txt').write_text('Local export stays here\n')
                env = os.environ.copy()
                env.update(SDL_VIDEODRIVER='dummy', CROSSINK_SIMULATOR_SMOKE_TEST='1',
                           CROSSINK_READING_TEST_SERVER=f'http://127.0.0.1:{server.server_port}')
                for key in tuple(env):
                    if key.startswith('CROSSINK_STATS_TEST_') or key.startswith('CROSSINK_READING_TEST_') and key != 'CROSSINK_READING_TEST_SERVER':
                        del env[key]
                if mode.startswith('folder-skip-'):
                    env['CROSSINK_READING_TEST_SKIP_FAILED'] = '1'
                    if mode == 'folder-skip-clippings':
                        env['CROSSINK_READING_TEST_SKIP_CLIPPINGS'] = '1'
                    if 'invalid' in mode:
                        env['CROSSINK_READING_TEST_INVALID_BOOK'] = '1'
                    if mode == 'folder-skip-all':
                        env['CROSSINK_READING_TEST_ALL_FAIL'] = '1'
                if mode in ('folder-xtc', 'all-books', 'stats-entry', 'stats-twice', 'xtc-book'):
                    env['CROSSINK_READING_TEST_XTC'] = '1'
                if mode in ('stats-entry', 'stats-twice'):
                    env['CROSSINK_READING_TEST_STATS_ENTRY'] = '1'
                if mode in ('stats-twice', 'clippings-twice', 'feed-twice', 'feed-parked', 'feed-batch-404', 'feed-batch-500'):
                    env['CROSSINK_READING_TEST_TWICE'] = '1'
                if mode.startswith('feed-'):
                    env['CROSSINK_READING_TEST_FEED'] = '1'
                if mode == 'feed-parked':
                    env['CROSSINK_READING_TEST_PARKED'] = '1'
                if mode == 'stats-twice':
                    env['CROSSINK_READING_TEST_MARK_PENDING'] = '/read/first.epub'
                if mode in ('clippings-only', 'clippings-twice', 'clippings-download'):
                    env['CROSSINK_READING_TEST_CLIPPINGS_ONLY'] = '1'
                if mode == 'xtc-book':
                    env['CROSSINK_READING_TEST_XTC_BOOK'] = '1'
                if mode in ('all-books', 'stats-entry', 'stats-twice'):
                    env['CROSSINK_READING_TEST_ALL'] = '1'
                if mode in ('disabled', 'cancel'):
                    env['CROSSINK_READING_TEST_' + mode.upper()] = '1'
                if mode == 'folder-done-touch':
                    env['CROSSINK_READING_TEST_DONE_TOUCH'] = '1'
                if mode == 'folder-done-confirm':
                    env['CROSSINK_READING_TEST_DONE_CONFIRM'] = '1'
                if mode.startswith('folder-ask') or mode.startswith('current-ask'):
                    env['CROSSINK_READING_TEST_ASK'] = '1'
                if mode == 'folder-ask-cancel':
                    env['CROSSINK_READING_TEST_ASK_CANCEL'] = '1'
                if mode.startswith('folder-ask-skip'):
                    env['CROSSINK_READING_TEST_ASK_SKIP'] = '1'
                if mode.endswith('-landscape'):
                    env['CROSSINK_READING_TEST_LANDSCAPE'] = '1'
                if mode == 'folder-ask-exit-held':
                    env['CROSSINK_READING_TEST_ASK_EXIT_HELD'] = '1'
                if mode == 'folder-exit-held':
                    env['CROSSINK_READING_TEST_EXIT_HELD'] = '1'
                variants = {'folder-no-progress': 'missing', 'folder-legacy-progress': 'legacy4',
                            'folder-zero-count': 'zero', 'folder-past-end': 'past-end'}
                if mode in variants:
                    env['CROSSINK_READING_TEST_PROGRESS_VARIANT'] = variants[mode]
                if mode == 'folder-empty':
                    env['CROSSINK_READING_TEST_EMPTY_FOLDER'] = '1'
                if mode == 'folder-no-stats':
                    env['CROSSINK_READING_TEST_NO_STATS'] = '1'
                if mode == 'folder-tracking-disabled':
                    env['CROSSINK_READING_TEST_TRACKING_DISABLED'] = '1'
                if mode == 'folder-missing-cache':
                    env['CROSSINK_READING_TEST_MISSING_CACHE'] = '1'
                if mode == 'auth':
                    env['CROSSINK_READING_TEST_ERROR'] = '1'
                if mode.startswith('current-'):
                    env['CROSSINK_READING_TEST_CURRENT'] = '1'
                if mode == 'current-upload-crosspoint':
                    env['CROSSINK_READING_TEST_SUPPORTED'] = '1'
                if mode.startswith('current-menu'):
                    env['CROSSINK_READING_TEST_MENU'] = '1'
                if mode == 'current-menu-cold':
                    env['CROSSINK_READING_TEST_COLD'] = '1'
                if mode == 'current-menu-kept':
                    env['CROSSINK_READING_TEST_MENU_KEPT'] = '1'
                if mode.startswith('matching-'):
                    env['CROSSINK_READING_TEST_MATCHING'] = mode.removeprefix('matching-')
                if mode.startswith('open-kept-'):
                    env['CROSSINK_READING_TEST_OPEN_KEPT'] = mode.removeprefix('open-kept-')
                result = subprocess.run([str(args.program.resolve())], cwd=directory, env=env,
                                        capture_output=True, text=True, timeout=120)
                log = result.stdout + result.stderr
                if result.returncode or 'Stats upload transport smoke passed' not in log:
                    print(log[-12000:])
                    raise RuntimeError(f'{mode}: simulator failed ({result.returncode})')
                if mode in ('folder-mixed', 'folder-ask', 'folder-equal', 'folder-ask-cancel', 'folder-ask-skip',
                            'folder-ask-skip-landscape', 'folder-ask-exit-held'):
                    cache = fs / (fs / 'first-cache.txt').read_text().lstrip('/')
                    saved = (cache / 'progress.bin').read_bytes()
                    page = int.from_bytes(saved[2:4], 'little')
                    assert (page > 2 if mode in ('folder-mixed', 'folder-ask') else page == 2), (mode, page)
                progress = [body for path, body in requests if path == '/syncs/progress']
                stats = [body for path, body in requests if path == '/api/v1/stats/books']
                clippings = [(path, body) for path, body in requests if path.startswith('/api/v1/clippings/')]
                globals_sent = [body for path, body in requests if path == '/api/v1/stats/global']
                assert mode == 'auth' or not probes, (mode, probes)
                if mode in ('clippings-download', 'folder-download'):
                    files = list((fs / '.crosspoint/clippings').glob('*.bin'))
                    imported = [f.read_bytes() for f in files if b'/read/unread.epub' in f.read_bytes()]
                    assert len(imported) == 1 and b'other-reader-highlight' in imported[0], files
                    assert b'Selection from another reader' in imported[0]
                    assert (fs / 'My Clippings.txt').read_text() == 'Local export stays here\n'
                    if mode == 'clippings-download':
                        assert not progress
                    else:
                        assert len(progress) == 2
                    assert 'clippingsFailed=0' in log, log[-10000:]
                elif mode in variants:
                    assert not progress and len(stats) == 2 and len(clippings) == 1 and len(globals_sent) == 1, requests
                    assert 'Finished: synced=0 skipped=3 failed=0' in log, log[-10000:]
                    assert 'Extras: global=0 stats=2 statsFailed=0 clippings=1 clippingsFailed=0' in log
                elif mode == 'folder-empty':
                    assert not progress and not stats and not clippings and len(globals_sent) == 1, requests
                elif mode in ('folder-no-stats', 'folder-tracking-disabled'):
                    assert len(progress) == 2 and not stats and not globals_sent and len(clippings) == 1, requests
                    assert 'Extras: global=7 stats=0 statsFailed=0 clippings=1 clippingsFailed=0' in log
                elif mode == 'folder-global-failure':
                    assert len(progress) == 2 and len(stats) == 2 and len(clippings) == 1 and len(globals_sent) == 1, requests
                    assert 'Finished: synced=2 skipped=1 failed=0' in log
                    assert 'Extras: global=6 stats=2 statsFailed=0 clippings=1 clippingsFailed=0' in log
                elif mode.startswith('folder-skip-'):
                    failed = 2 if mode == 'folder-skip-all' else 1
                    progress_synced = 0 if mode == 'folder-skip-all' else 1 if ('invalid' in mode or 'progress' in mode) else 2
                    assert f'Finished: synced={progress_synced} skipped=1 failed={failed}' in log, log[-10000:]
                    expected = {hashlib.md5(name.encode()).hexdigest() for name in ('first.epub', 'second.EPUB')}
                    assert len(globals_sent) == 1 and len(stats) == 2, requests
                    if 'invalid' in mode:
                        assert {b['document'] for b in progress} == {hashlib.md5(b'second.EPUB').hexdigest()}, requests
                    else:
                        assert {b['document'] for b in progress} == expected and len(progress) == 2, requests
                    assert len(clippings) == (2 if mode == 'folder-skip-clippings' else 1), requests
                    if mode == 'folder-skip-stats':
                        assert 'Extras: global=0 stats=1 statsFailed=1 clippings=1 clippingsFailed=0' in log
                    if mode == 'folder-skip-clippings':
                        assert 'Extras: global=0 stats=2 statsFailed=0 clippings=1 clippingsFailed=1' in log
                elif mode == 'current-menu-cold':
                    assert not progress and not stats and not clippings, requests
                    assert sum(path == '/api/v1/stats/global' for path, _ in requests) == 1
                elif mode.startswith('current-'):
                    # Ask on conflicts asks only when positions differ: no remote position
                    # uploads silently, and an equal one finishes as Already synced. A prompt
                    # would stop before the progress PUT and the stats/clippings below.
                    assert len(progress) == (1 if mode in ('current-upload', 'current-upload-crosspoint', 'current-ask-upload',
                                                           'current-menu', 'current-menu-kept') else 0), requests
                    # A confirmed CrossPoint Sync server gets this reader's own id; others the shared one.
                    reader_id = 'crossink-simulator' if mode == 'current-upload-crosspoint' else 'crossink-device'
                    assert all(body['device_id'] == reader_id for body in progress), progress
                    assert len(stats) == 1 and len(clippings) == 1, requests
                    expected = hashlib.md5(b'first.epub').hexdigest()
                    assert all(body['document'] == expected for body in progress)
                    assert stats[0]['items'][0]['document'] == expected
                    assert clippings[0][0].endswith(expected)
                    assert sum(path == '/api/v1/stats/global' for path, _ in requests) == 1
                elif mode == 'cancel':
                    assert not requests, requests
                elif mode.startswith('matching-'):
                    assert not requests and not gets, (requests, gets)
                elif mode.startswith('open-kept-'):
                    # Offering a kept position on open never touches the network. Accepting it
                    # maps the other reader's position (77.8% into chapter 1) onto this layout's
                    # 2-page chapter; staying keeps the saved page 2 of 10 (behind: chapter 2's start).
                    assert not requests and not gets, (requests, gets)
                    cache = fs / (fs / 'first-cache.txt').read_text().lstrip('/')
                    page = int.from_bytes((cache / 'progress.bin').read_bytes()[2:4], 'little')
                    applied = 'Applied kept position: spine=0 page=1/2' in log
                    assert applied == (mode == 'open-kept-accept'), log[-6000:]
                    assert page == (1 if applied else 0 if mode == 'open-kept-behind' else 2), (mode, page)
                elif mode in ('folder-ask-cancel', 'folder-ask-exit-held'):
                    # Choices come after the run: both books' extras and second.EPUB's position
                    # (no remote one) go first. Leaving at first.epub's choice sends nothing
                    # for its position and keeps the remote one for later.
                    second_id = hashlib.md5(b'second.EPUB').hexdigest()
                    assert len(globals_sent) == 1 and len(stats) == 2 and len(clippings) == 1, requests
                    assert [b['document'] for b in progress] == [second_id], requests
                    assert 'Reviewing 1 kept conflict(s)' in log and 'Finished:' not in log, log[-10000:]
                    assert (fs / '.crosspoint/sync_parked.bin').exists()
                    if mode == 'folder-ask-exit-held':
                        # Holding Exit through Upload local stops before that upload.
                        assert 'Bulk sync exited at: /read/first.epub' in log, log[-10000:]
                elif mode.startswith('folder-ask-skip'):
                    # Skip book keeps the batch going and each book's extras still send. Only the
                    # conflicting book asks; the book with no remote position uploads silently.
                    expected = {hashlib.md5(name.encode()).hexdigest() for name in ('first.epub', 'second.EPUB')}
                    assert [b['document'] for b in progress] == [hashlib.md5(b'second.EPUB').hexdigest()], requests
                    assert len(globals_sent) == 1 and len(clippings) == 1, requests
                    assert {b['items'][0]['document'] for b in stats} == expected and len(stats) == 2, requests
                    assert 'Finished: synced=1 skipped=2 failed=0' in log, log[-10000:]
                elif mode == 'folder-exit-held':
                    # Holding Exit as a Smart sync starts leaves before any book or overall upload.
                    assert not requests, requests
                    assert 'Finished:' not in log, log[-10000:]
                elif mode == 'auth':
                    # Rejected credentials fail each book, but the run reaches every book and
                    # ends on the summary instead of stopping at the first one.
                    assert len(globals_sent) == 1 and len(progress) == 2 and len(stats) == 2 and len(clippings) == 1, requests
                    assert 'Finished: synced=0 skipped=1 failed=2' in log, log[-10000:]
                elif mode == 'xtc-book':
                    # Sync Book on an XTC: overall plus that book's stats, no position or clippings.
                    assert not progress and not clippings and len(globals_sent) == 1 and len(stats) == 1, requests
                    assert stats[0]['items'][0]['document'] == hashlib.md5(b'comic.xtc').hexdigest()
                    assert 'Finished: synced=1 skipped=0 failed=0' in log, log[-10000:]
                elif mode in ('stats-entry', 'stats-twice'):
                    # Sync Reading Stats: overall stats plus every book's stats in one
                    # batched request, found by walking the card. No progress or clippings.
                    expected = {hashlib.md5(name.encode()).hexdigest() for name in ('first.epub', 'second.EPUB', 'comic.xtc')}
                    runs = 2 if mode == 'stats-twice' else 1
                    assert not progress and not clippings and len(globals_sent) == runs and len(stats) == runs, requests
                    assert {item['document'] for item in stats[0]['items']} == expected, requests
                    assert 'Extras: global=0 stats=3 statsFailed=0 clippings=0 clippingsFailed=0' in log, log[-10000:]
                    if mode == 'stats-twice':
                        # Caught up: only the book changed since the first sync.
                        assert [i['document'] for i in stats[1]['items']] == [hashlib.md5(b'first.epub').hexdigest()], requests
                elif mode in ('clippings-only', 'clippings-twice'):
                    # clippings-twice: a clipping saved after the first sync queues that book,
                    # so the second sync sends it again and nothing else.
                    # Clippings upload one request per clipping: 1 on the first sync, then
                    # both of first.epub's on the second.
                    runs = 3 if mode == 'clippings-twice' else 1
                    assert not progress and not stats and not globals_sent and len(clippings) == runs, requests
                    if mode == 'clippings-twice':
                        assert clippings[-1][1]['items'][0]['text'] == 'A later clipping', requests
                    assert all(path.endswith(hashlib.md5(b'first.epub').hexdigest()) for path, _ in clippings), requests
                    assert clippings[0][0].endswith(hashlib.md5(b'first.epub').hexdigest()), requests
                    assert 'Extras: global=7 stats=0 statsFailed=0 clippings=1 clippingsFailed=0' in log, log[-10000:]
                elif mode in ('folder-xtc', 'all-books'):
                    # XTC adds a stats upload without a position; Library scope also reaches
                    # books outside /read (outside.epub has no saved progress).
                    expected = {hashlib.md5(name.encode()).hexdigest() for name in ('first.epub', 'second.EPUB', 'comic.xtc')}
                    assert len(progress) == 2 and len(globals_sent) == 1 and len(clippings) == 1, requests
                    assert {b['items'][0]['document'] for b in stats} == expected and len(stats) == 3, requests
                    # The XTC counts as synced and unread.epub is skipped. Sync Books walks
                    # the card, so the deleted outside.epub is simply not there.
                    skipped = 1
                    assert f'Finished: synced=3 skipped={skipped} failed=0' in log, log[-10000:]
                    # XTC books have no position to pull, so they never enter the id map.
                    assert not any(p.endswith('.xtc') for p in read_sync_ids(fs)), read_sync_ids(fs)
                    assert 'Extras: global=0 stats=3 statsFailed=0 clippings=1 clippingsFailed=0' in log, log[-10000:]
                elif mode == 'unsupported':
                    # The first 404 marks the server progress-only: later books skip extras
                    # and the folder finishes instead of stopping on an "extra upload failed" error.
                    assert len(globals_sent) == 1 and len(progress) == 2 and not stats and not clippings, requests
                    assert 'Finished: synced=2 skipped=1 failed=0' in log, log[-10000:]
                    assert 'Extras: global=3 stats=0 statsFailed=0 clippings=0 clippingsFailed=0' in log, log[-10000:]
                else:
                    assert len(progress) == (1 if mode in ('folder-mixed', 'folder-equal', 'folder-ask') else 2), requests
                    assert all(0 < body['percentage'] < 1 and body['progress'] for body in progress)
                    # The overall stats upload runs first and confirms a CrossPoint Sync server, so
                    # progress then carries this reader's own id. With stats and clippings off the
                    # custom server is never confirmed and only gets the shared id.
                    reader_id = 'crossink-device' if mode == 'disabled' else 'crossink-simulator'
                    assert all(body['device_id'] == reader_id for body in progress), progress
                    # Synced EPUBs are recorded in the id map the progress pull uses.
                    ids = read_sync_ids(fs)
                    for book in progress:
                        path = next(p for p, d in ids.items() if p.endswith('/' + book['metadata']['filename']))
                        assert ids[path] == book['document'], (path, ids)
                    expected = {hashlib.md5(name.encode()).hexdigest() for name in ('first.epub', 'second.EPUB')}
                    assert {body['document'] for body in progress} == (expected - {hashlib.md5(b'first.epub').hexdigest()} if mode in ('folder-mixed', 'folder-equal', 'folder-ask') else expected)
                    if mode == 'disabled':
                        assert len(requests) == 2
                    else:
                        assert len(stats) == 2 and {b['items'][0]['document'] for b in stats} == expected
                        assert sum(path == '/api/v1/stats/global' for path, _ in requests) == 1
                        assert len(clippings) == 1
                        item = clippings[0][1]['items'][0]
                        assert len(item['id']) == 32 and all(c in '0123456789abcdef' for c in item['id'])
                        assert item['text'] == 'Quoted "text" — café\nnext line'
                        assert item['layout_signature'] == 1234
                        assert all(key not in item for key in ('note', 'color', 'deleted', 'para'))
                        assert clippings[0][0].endswith(hashlib.md5(b'first.epub').hexdigest())
                if mode in ('stats-twice', 'clippings-twice'):
                    # Both repeat runs clear their acknowledged pending bits.
                    assert 'Starting second sync' in log, log[-10000:]
                    if mode == 'stats-twice':
                        assert log.count('catch-up complete') == 1, log[-10000:]
                    # sync_pending.bin: version byte, entries of (mask, u16 path length, path), CRC32.
                    data = (fs / '.crosspoint/sync_pending.bin').read_bytes()[1:-4]
                    bit = 0x02 if mode == 'stats-twice' else 0x04
                    offset = 0
                    while offset < len(data):
                        mask, length = data[offset], int.from_bytes(data[offset + 1:offset + 3], 'little')
                        entry = data[offset + 3:offset + 3 + length].decode()
                        assert not mask & bit, ('pending bit not cleared', entry, mask)
                        offset += 3 + length
                if mode.startswith('folder-ask-skip'):
                    # Skip book keeps the unreconciled remote position for a later push to compare with.
                    parked = (fs / '.crosspoint/sync_parked.bin').read_bytes()
                    used = [parked[i:i + 384] for i in range(0, len(parked), 384) if parked[i] == 1]
                    assert len(used) == 1 and used[0][4:20] == hashlib.md5(b'first.epub').digest(), parked[:40]
                if mode.startswith('feed-'):
                    feed = [path for _, path in gets if path.startswith('/api/v1/progress/changes')]
                    # The cursor is read once, before the first walk; an older server's 404 falls back.
                    assert feed == ['/api/v1/progress/changes?since=0&limit=0&device=crossink-simulator'], gets
                    first_walk = 'First walk of /read complete' in log
                    assert first_walk == (mode != 'feed-404'), log[-10000:]
                if mode in ('feed-twice', 'feed-parked', 'feed-batch-404', 'feed-batch-500'):
                    assert 'Starting second sync' in log, log[-10000:]
                    # The second run never asks per book: one feed page, then only the pending push.
                    assert not any(m == 'GET' and p.startswith('/syncs/progress') for m, p in second), second
                    feed = [path for method, path in second if method == 'GET' and 'progress/changes' in path]
                    assert feed == ['/api/v1/progress/changes?since=5&limit=10&device=crossink-simulator'], second
                    pushed = [body for path, body in second if path == '/syncs/progress']
                    batches = [body for path, body in second if path == '/api/v1/progress/batch']
                    second_id = hashlib.md5(b'second.EPUB').hexdigest()
                    if mode == 'feed-twice':
                        # The push sends its positions in one batch request, not one PUT per book.
                        assert not pushed and len(batches) == 1, second
                        items = batches[0]['items']
                        assert [i['document'] for i in items] == [second_id], batches
                        assert items[0]['device_id'] == 'crossink-simulator' and items[0]['progress'], items
                        assert 'Batch progress upload: 1 books' in log, log[-10000:]
                    elif mode == 'feed-batch-500':
                        # A failed batch fails each of its books, which stay pending for the next sync.
                        assert not pushed and len(batches) == 1, second
                        assert 'Book sync failed (parts 0x1): /read/sub/second.EPUB' in log, log[-10000:]
                        pending = (fs / '.crosspoint/sync_pending.bin').read_bytes()
                        assert b'/read/sub/second.EPUB' in pending, pending
                    elif mode == 'feed-batch-404':
                        # A server without the batch route: one PUT per book for the rest of the run.
                        assert len(batches) == 1 and [b['document'] for b in pushed] == [second_id], second
                        assert 'No batch progress route' in log, log[-10000:]
                    else:
                        # The parked position was further ahead: applied, nothing uploaded.
                        assert not pushed and not batches, second
                        assert 'Comparing with a parked remote position: /read/sub/second.EPUB' in log, log[-10000:]
                    assert 'Pull done: applied=1 parked=1' in log, log[-10000:]
                    # Applying first.epub's pulled position left its pending stats for the push.
                    stats_docs = {i['document'] for p, b in second if p == '/api/v1/stats/books' for i in b['items']}
                    assert hashlib.md5(b'first.epub').hexdigest() in stats_docs, second
                    # The pulled position was applied to first.epub (page 7 of 10).
                    cache = fs / (fs / 'first-cache.txt').read_text().lstrip('/')
                    assert (cache / 'progress.bin').read_bytes() != snapshot['first'], 'pulled position not applied'
                    # One parked slot: the unknown book (second.EPUB's was resolved).
                    parked = (fs / '.crosspoint/sync_parked.bin').read_bytes()
                    slots = [parked[i:i + 384] for i in range(0, len(parked), 384)]
                    used = [slot for slot in slots if slot[0] == 1]
                    assert len(used) == 1 and used[0][4:20] == bytes.fromhex('ab' * 16), parked[:40]
                print(f'PASS: {mode} reading sync activity, request scope and payloads')
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
