#!/usr/bin/env python3
"""Wrap a compatible Hypher trie for installation from SD."""

import argparse
from pathlib import Path
import re
import zlib

import package_hyphenation as legacy

MAX_SOURCE_SIZE = 256 * 1024


def wrap_trie(code: str, raw: bytes) -> bytes:
    if not re.fullmatch(r"[a-z]{2}", code) or code == "en":
        raise ValueError("Use a lowercase two-letter language code other than en")
    payload = raw[4:]
    offset = int.from_bytes(raw[:4], "big") - 4
    if not 0 <= offset < len(payload):
        raise ValueError("Invalid Hypher trie root offset")
    if legacy.PACK_HEADER.size + len(payload) > MAX_SOURCE_SIZE:
        raise ValueError("Pack exceeds the 256 KiB source-file limit")
    return legacy.PACK_HEADER.pack(b"CPHY", 1, code.encode("ascii"), 2, 2, 0, 0,
                                   offset, len(payload), zlib.crc32(payload)) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("code", help="Two-letter language code matching the input trie")
    parser.add_argument("trie", type=Path, help="Path to a Hypher .bin trie for a 2/2 language")
    parser.add_argument("--output-dir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        pack = wrap_trie(args.code, args.trie.read_bytes())
        args.output_dir.mkdir(parents=True, exist_ok=True)
        destination = args.output_dir / f"hyph-{args.code}.cphyph"
        destination.write_bytes(pack)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Error: {error}\n")
    print(destination)


if __name__ == "__main__":
    main()
