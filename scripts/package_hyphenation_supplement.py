"""Package pinned supplemental tries without modifying the schema-1 or -2 assets."""

import hashlib
import json
from pathlib import Path
import re
import zlib

import package_hyphenation as legacy
from package_hyphenation_v2 import artifacts as base_artifacts

EXTRA = Path("assets/hyphenation-extra")
SOURCE_PREFIX = "hyphenation-sources/extra/"


def artifacts(root=legacy.ROOT):
    sources = {path.relative_to(root / EXTRA).as_posix(): path.read_bytes()
               for path in sorted((root / EXTRA).rglob("*")) if path.is_file()}
    pinned = json.loads(sources["packs.json"])
    entries = {SOURCE_PREFIX + name: data for name, data in sources.items()}
    entries[SOURCE_PREFIX + "package_hyphenation_supplement.py"] = (root / "scripts/package_hyphenation_supplement.py").read_bytes()
    entries[SOURCE_PREFIX + "package_hyphenation.py"] = (root / "scripts/package_hyphenation.py").read_bytes()
    for code, info in sorted(pinned["packs"].items()):
        if not re.fullmatch(r"[a-z]{2}", code) or code in (*legacy.CODES, "en", "nl"):
            raise ValueError("Unexpected supplemental code: " + code)
        trie = f"tries/{code}.bin"
        for name in (trie, info["pattern"]):
            if hashlib.sha256(sources[name]).hexdigest() != info["sha256"][name]:
                raise ValueError("Source does not match pinned snapshot: " + name)
        raw = sources[trie]
        payload = raw[4:]
        offset = int.from_bytes(raw[:4], "big") - 4
        if not 0 <= offset < len(payload) or len(payload) + legacy.PACK_HEADER.size > 262144:
            raise ValueError("Supplemental trie outside bounds: " + code)
        entries[f".crossink/hyphenation/hyph-{code}.cphyph"] = legacy.PACK_HEADER.pack(
            b"CPHY", 1, code.encode("ascii"), 2, 2, 0, 0, offset, len(payload), zlib.crc32(payload)) + payload
    return pinned["packs"], entries


def release_artifacts(root=legacy.ROOT):
    entries = base_artifacts(root)
    manifest = json.loads(entries.pop("hyphenation-manifest.json"))
    packs, extra = artifacts(root)
    if entries.keys() & extra.keys():
        raise ValueError("Supplemental assets overlap the existing bundle")
    entries.update(extra)
    manifest["languages"] = sorted([*manifest["languages"], *packs])
    manifest["source"] = "CrossInk checked-in tables and pinned Hypher tries"
    manifest["sha256"] = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())}
    entries["hyphenation-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return entries
