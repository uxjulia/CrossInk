#!/usr/bin/env python3
"""Verify distributable packs and run them through the actual flash store."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("packs", root / "scripts/package_hyphenation.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
entries = module.artifacts(root)
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
assert len([name for name in entries if name.endswith(".tex")]) == 9
print("Nine reproducible real packs, checksums, notices and flash installation passed")
