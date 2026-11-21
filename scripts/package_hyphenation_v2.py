"""Extend the legacy external pack set without changing its bytes or sources."""

import hashlib
import json
from pathlib import Path
import zlib

import package_hyphenation as legacy

CODES = (*legacy.CODES, "nl")
DUTCH_ASSETS = Path("assets/hyphenation-v2")
DUTCH_TRIE = DUTCH_ASSETS / "tries/nl.bin"
DUTCH_SOURCE = DUTCH_ASSETS / "patterns/hyph-nl.tex"
DUTCH_LICENSE = DUTCH_ASSETS / "LICENSE-MIT"
DUTCH_NOTICE = DUTCH_ASSETS / "README.md"
DUTCH_FILES = (DUTCH_SOURCE, DUTCH_TRIE, DUTCH_LICENSE, DUTCH_NOTICE)
DUTCH_PACK = ".crossink/hyphenation/hyph-nl.cphyph"
DUTCH_TRIE_SHA256 = "1177ef126b31bbb30f080633680fc14cc4cd6f56d1597981c975389b861d978b"
DUTCH_SOURCE_SHA256 = "d21499bfbee53e4d50e867ef92a41fd28b65725d7c91c980e4f8f02889bf6b2a"


def dutch_pack(root=legacy.ROOT):
    trie = (root / DUTCH_TRIE).read_bytes()
    source = (root / DUTCH_SOURCE).read_bytes()
    if (hashlib.sha256(trie).hexdigest() != DUTCH_TRIE_SHA256
            or hashlib.sha256(source).hexdigest() != DUTCH_SOURCE_SHA256):
        raise ValueError("Dutch patterns do not match the pinned Hypher snapshot")
    offset = int.from_bytes(trie[:4], "big") - 4
    payload = trie[4:]
    if not 0 <= offset < len(payload):
        raise ValueError("Invalid Dutch trie bounds")
    return legacy.PACK_HEADER.pack(b"CPHY", 1, b"nl", 2, 2, 0, 0, offset, len(payload),
                                   zlib.crc32(payload)) + payload


def artifacts(root=legacy.ROOT):
    entries = legacy.artifacts(root)
    manifest = json.loads(entries.pop("hyphenation-manifest.json"))
    entries[DUTCH_PACK] = dutch_pack(root)
    for source in DUTCH_FILES:
        entries[f"hyphenation-sources/dutch/{source.relative_to(DUTCH_ASSETS)}"] = (root / source).read_bytes()
    manifest["languages"] = CODES
    manifest["source"] = "CrossInk checked-in generated tables and pinned Hypher Dutch trie"
    manifest["sha256"] = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())}
    entries["hyphenation-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return entries
