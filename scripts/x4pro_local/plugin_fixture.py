#!/usr/bin/env python3
"""Create a disposable plugin SD seed and serve fixtures for actual QEMU firmware."""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading
import time
import zipfile
import zlib

from core import command, fixture, sha, write_json


def prepare(root, port):
    root = Path(root)
    image = fixture(root / "fixture", "reading", network=True)
    origin = f"http://10.0.2.2:{port}"
    key = bytes(range(32))  # Public synthetic test key; never use for real books.
    iv = bytes(range(16))
    book = root / "protected.epub"
    with zipfile.ZipFile(root / "fixture/files/TestBook.epub") as source, zipfile.ZipFile(book, "w") as target:
        for info in source.infolist():
            data = source.read(info.filename)
            if info.filename == "OEBPS/chapter.xhtml":
                original = len(data)
                compressor = zlib.compressobj(wbits=-15)
                compressed = compressor.compress(data) + compressor.flush()
                encrypted = subprocess.run(
                    ["openssl", "enc", "-aes-256-cbc", "-K", key.hex(), "-iv", iv.hex()],
                    input=compressed, capture_output=True, check=True, timeout=10).stdout
                data = iv + encrypted
                info.compress_type = zipfile.ZIP_STORED
            target.writestr(info, data)
        target.writestr("META-INF/encryption.xml", '<encryption><EncryptedData>'
            '<EncryptionMethod Algorithm = "http://www.w3.org/2001/04/xmlenc#aes256-cbc"/>'
            '<CipherData><CipherReference URI = "OEBPS/chapter.xhtml"/></CipherData>'
            '<EncryptionProperties><EncryptionProperty>'
            f'<Compression Method="8" OriginalLength="{original}"/>'
            '</EncryptionProperty></EncryptionProperties></EncryptedData></encryption>')
    write_json(root / "key-request.json", dict(path="/Books/Network AES256.epub",
               key=base64.b64encode(key).decode(), expires=0))
    tree = root / "extra"
    plugin = tree / "plugins/network"
    plugin.mkdir(parents=True)
    write_json(plugin / "manifest.json", dict(title="Network test", description="Synthetic emulator validation"))
    fields = dict(title="title", id="id", version="version", url="url", description="description",
                  detail_author="author", repository="repository", details_url="details.url", details_format="details.format")
    events = {name: dict(connect=True, request=dict(url=origin + "/events", method="POST",
              body=name + ":{event.id}:{event.book}:{event.path}:{event.ts}"))
              for name in ("reader.open", "reader.exit", "reader.session", "book.downloaded", "sleep.enter")}
    write_json(plugin / "device.json", dict(title="Network test",
        browse=dict(url=origin + "/catalog", fields=fields, compatibility=dict(requires="requires")),
        download=dict(url="{url}", filename="{title}.epub", dest_dir="/Books"), events=events))
    settings = tree / ".crosspoint"
    settings.mkdir()
    write_json(settings / "crossink-settings.json", dict(pluginsEnabled=1, uiTheme=1,
               sleepTimeoutMinutes=31, textAntiAliasing=0, hideClock=2))
    command(["mcopy", "-o", "-s", "-i", str(image) + "@@1048576", *tree.iterdir(), "::"])
    write_json(root / "plugin-fixture.json", dict(sd_sha256=sha(image), book_sha256=sha(book),
               origin=origin, key_request="key-request.json", synthetic=True))
    return image


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, root, port):
        self.root = Path(root)
        self.lock = threading.Lock()
        self.event_attempts = 0
        super().__init__(("127.0.0.1", port), Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, data, status=200, kind="application/json", body=""):
        with self.server.lock:
            with (self.server.root / "requests.jsonl").open("a") as stream:
                stream.write(json.dumps(dict(method=self.command, path=self.path, body=body,
                                            status=status, time=time.time())) + "\n")
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", kind)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        origin = f"http://10.0.2.2:{self.server.server_port}"
        if self.path == "/catalog":
            shared = dict(title="Network AES256", author="a" * 1900, repository="r" * 1900)
            rows = [dict(shared, id="filtered", version="0", requires="catalog.missing",
                         description="wrong " * 300, details=dict(url=origin + "/wrong-details", format="html")),
                    dict(shared, id="aes256", version="1", url=origin + "/protected.epub",
                         description="Selected encrypted book. " + "x" * 1800,
                         details=dict(url=origin + "/details", format="html"))]
            self.reply(json.dumps(rows).encode())
        elif self.path == "/details":
            self.reply(b"<h1>Network catalog details</h1><p>Selected the correct item after a filtered duplicate.</p>"
                       b"<p>AES-256 download through emulated Wi-Fi.</p>", kind="text/html")
        elif self.path == "/protected.epub":
            self.reply((self.server.root / "protected.epub").read_bytes(), kind="application/epub+zip")
        else:
            self.reply(b"", 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 <= length <= 8192:
            self.close_connection = True
            self.reply(b"", 413)
            return
        body = self.rfile.read(length).decode(errors="replace")
        if self.path == "/events":
            with self.server.lock:
                self.server.event_attempts += 1
                status = 503 if self.server.event_attempts == 1 else 200
            self.reply(b"", status, body=body)
        else:
            self.reply(b"", 404, body=body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New disposable directory")
    parser.add_argument("--port", type=int, default=18081)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    args.output.mkdir(parents=True, exist_ok=False)
    # Reserve the port before generating a seed that refers to it.
    with FixtureServer(args.output, args.port) as server:
        image = prepare(args.output, args.port)
        print(f"SD seed: {image}\nKey request: {args.output / 'key-request.json'}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
