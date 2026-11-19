#!/usr/bin/env python3
"""Generate a small deterministic EPUB for real firmware QEMU CSS reopen tests."""
import sys
from zipfile import ZipFile, ZIP_STORED

path = sys.argv[1] if len(sys.argv) > 1 else "css-rule-cap.epub"
with ZipFile(path, "w", ZIP_STORED) as book:
    book.writestr("mimetype", "application/epub+zip")
    book.writestr("META-INF/container.xml", '<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
    book.writestr("book.opf", '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">css-rule-cap</dc:identifier><dc:title>CSS rule cap regression</dc:title><dc:language>en</dc:language></metadata><manifest><item id="text" href="text.xhtml" media-type="application/xhtml+xml"/><item id="css" href="style.css" media-type="text/css"/></manifest><spine><itemref idref="text"/></spine></package>')
    book.writestr("style.css", "".join(f".c{i}{{font-weight:bold;}}\n" for i in range(1501)))
    book.writestr("text.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Cache test</title><link rel="stylesheet" href="style.css"/></head><body><p class="c0">First retained rule should be bold.</p><p class="c1499">Last retained rule should be bold.</p><p class="c1500">Beyond the cap should remain regular.</p></body></html>')
print(path)
