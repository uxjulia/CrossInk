#!/usr/bin/env python3
"""Exercise GIF and cover-overlay rendering with an isolated simulator SD card.

Requires Pillow and an existing simulator build. Does not emulate e-ink waveforms.
"""
from pathlib import Path
from PIL import Image, ImageDraw
import os
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='crossink-upstream-features-') as directory:
    work = Path(directory)
    sd = work / 'fs_'
    sd.mkdir()
    cover = Image.new('RGB', (80, 80), 'white')
    ImageDraw.Draw(cover).rectangle((40, 0, 79, 79), fill='black')
    cover.save(sd / 'cover.gif', interlace=False)
    Image.new('RGB', (80, 80), (128, 128, 128)).save(sd / 'midtone.gif', interlace=False)
    for suffix in ('bmp', 'png'):
        overlay = Image.new('RGBA' if suffix == 'png' else 'RGB', (480, 800),
                            (255, 255, 255, 0) if suffix == 'png' else 'white')
        ImageDraw.Draw(overlay).rectangle((110, 390, 130, 410), fill=(128, 128, 128) if suffix == 'bmp' else 'black')
        overlay.save(sd / ('overlay.' + suffix))
    with zipfile.ZipFile(sd / 'port.epub', 'w') as book:
        book.writestr('mimetype', 'application/epub+zip')
        book.writestr('META-INF/container.xml', '<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        book.writestr('content.opf', '''<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">crossink-gif-port-fixture</dc:identifier><dc:title>GIF Port</dc:title><dc:language>en</dc:language><meta name="cover" content="cover"/></metadata><manifest><item id="cover" href="cover.gif" media-type="image/gif" properties="cover-image"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>''')
        book.writestr('chapter.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>GIF image fixture.</p><img src="cover.gif"/></body></html>')
        book.write(sd / 'cover.gif', 'cover.gif')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               CROSSINK_SIMULATOR_SMOKE_TEST='1', CROSSINK_SIMULATOR_SMOKE_BOOK='/port.epub', CROSSINK_SIMULATOR_SMOKE_UPSTREAM_FEATURES='1')
    result = subprocess.run([str(root / '.pio/build/simulator/program')], cwd=work, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    print(result.stdout)
    raise SystemExit(result.returncode)
