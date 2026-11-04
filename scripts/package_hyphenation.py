#!/usr/bin/env python3
"""Create CPHY v1 packs from CrossInk's checked-in, generated pattern snapshot.

The binary payload is exactly the old firmware table; no network or TeX compiler
is needed to package a release. English remains embedded. Format compatibility
follows CrossPoint Reader PR #3706 (MIT).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
CODES = ("de", "es", "fr", "it", "pl", "pt", "ru", "sv", "uk")
PACK_HEADER = struct.Struct("<4sB2sBBBHIII")


def pack_bytes(code, root=ROOT):
    source = (root / f"lib/Epub/Epub/hyphenation/generated/hyph-{code}.trie.h").read_text()
    array = re.search(rf"{code}_trie_data\[\]\s*=\s*\{{(.*?)\}};", source, re.S)
    descriptor = re.search(rf"{code}_patterns\s*=\s*\{{\s*(0x[0-9A-Fa-f]+)u,", source)
    if not array or not descriptor:
        raise ValueError(f"Unrecognized generated trie: {code}")
    payload = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})\b", array[1]))
    root_offset = int(descriptor[1], 16)
    if not payload or root_offset >= len(payload):
        raise ValueError(f"Invalid trie bounds: {code}")
    return PACK_HEADER.pack(b"CPHY", 1, code.encode("ascii"), 2, 2, 0, 0,
                            root_offset, len(payload), zlib.crc32(payload)) + payload


def artifacts(root=ROOT):
    entries = {f".crossink/hyphenation/hyph-{code}.cphyph": pack_bytes(code, root) for code in CODES}
    for source in sorted((root / "assets/hyphenation").rglob("*")):
        if source.is_file():
            entries[f"hyphenation-sources/{source.relative_to(root / 'assets/hyphenation')}"] = source.read_bytes()
    entries["hyphenation-manifest.json"] = (json.dumps({
        "format": "CPHY v1", "languages": CODES,
        "source": "CrossInk checked-in generated hyphenation tables",
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())},
    }, indent=2) + "\n").encode()
    return entries


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(artifacts().items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    print(f"Packaged {len(CODES)} hyphenation languages: {args.output}")
