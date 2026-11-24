#!/usr/bin/env python3
"""Verify distributable packs and run them through the actual flash store."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts"))
import package_hyphenation as legacy
import package_hyphenation_v2 as module

entries = module.artifacts(root)
old = legacy.artifacts(root)
assert module.CODES == (*legacy.CODES, "nl")
assert all(entries[name] == value for name, value in old.items() if name != "hyphenation-manifest.json")
assert module.DUTCH_PACK not in old
assert "hyphenation-sources/dutch/LICENSE-MIT" in entries
assert not any(name.startswith(".crosspoint/") for name in entries)
assert entries == module.artifacts(root)
manifest = json.loads(entries["hyphenation-manifest.json"])
for name, digest in manifest["sha256"].items():
    assert hashlib.sha256(entries[name]).hexdigest() == digest
with tempfile.TemporaryDirectory() as temp:
    paths = []
    for code in module.CODES:
        name = f".crossink/hyphenation/hyph-{code}.cphyph"
        data = entries[name]
        magic, version, identity, left, right, flags, reserved, root_offset, size, crc = struct.unpack("<4sB2sBBBHIII", data[:24])
        assert (magic, version, identity, left, right, flags, reserved) == (b"CPHY", 1, code.encode(), 2, 2, 0, 0)
        assert size == len(data) - 24 and root_offset < size
        assert crc == zlib.crc32(data[24:])
        path = Path(temp) / f"hyph-{code}.cphyph"
        path.write_bytes(data)
        paths.append(str(path))
    subprocess.run([sys.argv[2], *paths], check=True)
assert ".crossink/hyphenation/hyph-en.cphyph" not in entries
assert len([name for name in entries if name.endswith(".tex")]) == 10
print("Ten reproducible real packs, checksums, notices and flash installation passed")


import zipfile
import package_hyphenation_supplement as supplement

packs, extra = supplement.artifacts(root)
all_entries = supplement.release_artifacts(root)
manifest = json.loads(all_entries["hyphenation-manifest.json"])
assert len(packs) == 18 and not set(packs) & set(module.CODES)
assert set(manifest["languages"]) == set(packs) | set(module.CODES)
assert all(all_entries[name] == value for name, value in entries.items() if name != "hyphenation-manifest.json")
for name, digest in manifest["sha256"].items():
    assert hashlib.sha256(all_entries[name]).hexdigest() == digest
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    archive = temp / "test-languages.zip"
    subprocess.run([sys.executable, root / "scripts/package_languages.py", "--version", "2.1.0-test",
                    "--output", archive], check=True)
    with zipfile.ZipFile(archive) as bundle:
        for name, data in all_entries.items():
            assert bundle.read(name) == data
    paths = []
    for code in packs:
        filename = f"hyph-{code}.cphyph"
        path = temp / filename
        path.write_bytes(extra[f".crossink/hyphenation/{filename}"])
        paths.append(path)
    subprocess.run([sys.argv[2], "--supplemental", *paths], check=True)
