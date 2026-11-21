#!/usr/bin/env python3
"""Wrap a compatible Hypher trie for SD, and assemble the release pack set."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import zlib

import package_hyphenation as legacy

EXTRA = Path("assets/hyphenation-extra")
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


def release_artifacts(root=legacy.ROOT):
    """Add SD-only material without changing the nine-entry network catalog."""
    entries = legacy.artifacts(root)
    manifest = json.loads(entries.pop("hyphenation-manifest.json"))
    sources = {path.relative_to(root / EXTRA).as_posix(): path.read_bytes()
               for path in sorted((root / EXTRA).rglob("*")) if path.is_file()}
    pinned = json.loads(sources["packs.json"])
    for code, info in sorted(pinned["packs"].items()):
        if code in legacy.CODES:
            raise ValueError(f"SD pack would replace a legacy download: {code}")
        trie = f"tries/{code}.bin"
        for name in (trie, info["pattern"]):
            if hashlib.sha256(sources[name]).hexdigest() != info["sha256"][name]:
                raise ValueError(f"Source does not match pinned snapshot: {name}")
        entries[f".crossink/hyphenation/hyph-{code}.cphyph"] = wrap_trie(code, sources[trie])
    entries.update({f"hyphenation-sources/extra/{name}": data for name, data in sources.items()})
    entries["hyphenation-sources/extra/package_hyphenation_sd.py"] = Path(__file__).read_bytes()
    entries["hyphenation-sources/extra/package_hyphenation.py"] = (root / "scripts/package_hyphenation.py").read_bytes()
    manifest["languages"] = sorted([*legacy.CODES, *pinned["packs"]])
    manifest["source"] = "CrossInk checked-in tables and pinned Hypher SD-only tries"
    manifest["sha256"] = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())}
    entries["hyphenation-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return entries


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
