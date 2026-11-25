#!/usr/bin/env python3
"""Generate a deterministic repeated-selector EPUB for C3 CSS-cache profiling."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--rules", type=int, default=1000)
parser.add_argument("--paragraphs", type=int, default=800)
args = parser.parse_args()
if not 4 <= args.rules <= 1400 or not 1 <= args.paragraphs <= 10000:
    parser.error("rules must be 4..1400 and paragraphs 1..10000")
css = "p { margin: 0; } .body { text-align: justify; } span { font-style: italic; } .repeat { font-weight: bold; }\n"
css += "\n".join(f".unused{i} {{ margin-top: {i % 7}px; }}" for i in range(args.rules - 4))
paragraphs = "\n".join(
    f'<p class="body">Paragraph {i}: A warm CSS cache should reuse the same paragraph and inline rules. '
    '<span class="repeat">The same selectors repeat throughout this chapter.</span> '
    'This fixture measures chapter layout, not panel refresh time.</p>'
    for i in range(args.paragraphs)
)
files = {
    "mimetype": "application/epub+zip",
    "META-INF/container.xml": '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
    "OEBPS/content.opf": '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">crossink-css-disk-memo</dc:identifier><dc:title>CSS disk memo profiling</dc:title><dc:language>en</dc:language><meta property="dcterms:modified">2026-11-17T00:00:00Z</meta></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="css" href="style.css" media-type="text/css"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest><spine><itemref idref="chapter"/></spine></package>',
    "OEBPS/nav.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">Repeated selectors</a></li></ol></nav></body></html>',
    "OEBPS/style.css": css,
    "OEBPS/chapter.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Repeated selectors</title><link rel="stylesheet" href="style.css" type="text/css"/></head><body>' + paragraphs + '</body></html>',
}
args.output.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(args.output, "w") as archive:
    for name, contents in files.items():
        # Stable ZIP timestamps make the fixture reproducible across invocations.
        info = ZipInfo(name, (2026, 11, 17, 0, 0, 0))
        info.compress_type = ZIP_STORED if name == "mimetype" else ZIP_DEFLATED
        archive.writestr(info, contents.encode())
print(args.output)
