#!/usr/bin/env python3
"""Generate the publisher-font simulator fixture using the bundled licensed fonts."""

import argparse
from pathlib import Path
from uuid import UUID
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--budget-case", action="store_true", help="Two 2 MiB body faces before an accent face")
    parser.add_argument("--adobe-obfuscation", action="store_true", help="Obfuscate publisher fonts with the Adobe UUID method")
    args = parser.parse_args()
    entries = {'mimetype': 'application/epub+zip',
     'META-INF/container.xml': '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" '
                               'version="1.0"><rootfiles><rootfile full-path="OEBPS/content.opf" '
                               'media-type="application/oebps-package+xml"/></rootfiles></container>',
     'OEBPS/content.opf': '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
                          'unique-identifier="id"><metadata '
                          'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier '
                          'id="id">publisher-font-port</dc:identifier><dc:title>Publisher '
                          'Fonts</dc:title><dc:language>en</dc:language></metadata><manifest><item '
                          'id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item '
                          'id="css" href="style.css" media-type="text/css"/><item id="body" '
                          'href="body.ttf" media-type="font/ttf"/><item id="accent" href="accent.ttf" '
                          'media-type="font/ttf"/></manifest><spine><itemref '
                          'idref="chapter"/></spine></package>',
     'OEBPS/style.css': "@font-face {font-family:'Publisher Body';src:url('body.ttf')} @font-face "
                        "{font-family:'Publisher Accent';src:url('accent.ttf')} p "
                        "{font-family:'Publisher Body';text-align:left} "
                        "h1,.accent{font-family:'Publisher Accent'}",
     'OEBPS/chapter.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Publisher '
                            'Fonts</title><link rel="stylesheet" '
                            'href="style.css"/></head><body><h1>Publisher heading</h1><p>The reader '
                            'body font stays selected in accents mode. <span class="accent">This '
                            'phrase uses an accent font.</span> The body continues here.</p><p>A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. A '
                            'readable page with embedded publisher fonts and normal body text. '
                            '</p></body></html>'}
    if args.budget_case:
        entries['OEBPS/style.css'] = entries['OEBPS/style.css'].replace(
            "@font-face {font-family:'Publisher Accent'",
            "@font-face {font-family:'Publisher Body';font-weight:bold;src:url('body-bold.ttf')} "
            "@font-face {font-family:'Publisher Accent'")
        entries['OEBPS/content.opf'] = entries['OEBPS/content.opf'].replace(
            '</manifest>', '<item id="body-bold" href="body-bold.ttf" media-type="font/ttf"/></manifest>')
    uuid = UUID("53124cf5-7195-45b2-b3a0-7f2974bc2b56")
    if args.adobe_obfuscation:
        entries['OEBPS/content.opf'] = entries['OEBPS/content.opf'].replace(
            'publisher-font-port', 'urn:uuid:' + str(uuid))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, "w", compression=ZIP_DEFLATED) as archive:
        font_paths = []

        def write_font(path, data):
            if args.adobe_obfuscation:
                prefix = bytes(value ^ uuid.bytes[i % 16] for i, value in enumerate(data[:1024]))
                data = prefix + data[1024:]
                font_paths.append(path)
            archive.writestr(path, data)

        for name, text in entries.items():
            archive.writestr(name, text, compress_type=ZIP_STORED if name == "mimetype" else ZIP_DEFLATED)
        fonts = ROOT / "lib/EpdFont/scalableFonts"
        if args.budget_case:
            # Trailing padding is valid SFNT data and exercises the loader's file-byte budget.
            for source, target in [("bitter_regular.ttf", "body.ttf"), ("bitter_bold.ttf", "body-bold.ttf")]:
                font = (fonts / source).read_bytes()
                assert len(font) <= 2 * 1024 * 1024
                write_font("OEBPS/" + target, font.ljust(2 * 1024 * 1024, b"\0"))
        else:
            write_font("OEBPS/body.ttf", (fonts / "bitter_regular.ttf").read_bytes())
        write_font("OEBPS/accent.ttf", (ROOT / "lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf").read_bytes())
        if font_paths:
            archive.writestr("META-INF/encryption.xml",
                '<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container" '
                'xmlns:e="http://www.w3.org/2001/04/xmlenc#">' + ''.join(
                    '<e:EncryptedData><e:EncryptionMethod Algorithm="http://ns.adobe.com/pdf/enc#RC"/>'
                    '<e:CipherData><e:CipherReference URI="' + path + '"/></e:CipherData></e:EncryptedData>'
                    for path in font_paths) + '</encryption>')


if __name__ == "__main__":
    main()
