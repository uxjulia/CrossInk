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
import zipfile

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
sys.path.insert(0, str(root / "scripts"))
import package_hyphenation_sd as sd

extra = "bg ca cs da fi gl hr is ku la lt nl no sl sq".split()
expanded = sd.release_artifacts(root)
assert expanded == sd.release_artifacts(root)
assert all(expanded[name] == data for name, data in entries.items() if name != "hyphenation-manifest.json")
manifest = json.loads(expanded["hyphenation-manifest.json"])
assert set(manifest["languages"]) == set(module.CODES).union(extra)
for name, digest in manifest["sha256"].items():
    assert hashlib.sha256(expanded[name]).hexdigest() == digest

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    archive = temp / "rc.zip"
    subprocess.run([sys.executable, root / "scripts/package_languages.py", "--version", "2.0.0-test",
                    "--output", archive], check=True)
    with zipfile.ZipFile(archive) as bundle:
        for name, data in expanded.items():
            assert bundle.read(name) == data
    paths = []
    for code in extra:
        filename = f"hyph-{code}.cphyph"
        path = temp / filename
        path.write_bytes(expanded[f".crossink/hyphenation/{filename}"])
        paths.append(path)
    subprocess.run([sys.argv[2], "--sd-packs", *paths], check=True)

# These pinned official CrossPoint assets were also exercised in eInk-QEMU.
for code, expected in {
    "da": "8341dacb3bdd700f711da687a5cee9d190f893e393ca89b1d0b671cd5411b57d",
    "fi": "6795e11871bdce7ae46333bbcdb9fd73d2a2eaa9752301f9aa02f5edcb8e9246",
}.items():
    assert manifest["sha256"][f".crossink/hyphenation/hyph-{code}.cphyph"] == expected

for code, raw in (("en", b"12345"), ("da", b"1234"), ("da", b"\0\0\0\4" + b"x" * sd.MAX_SOURCE_SIZE)):
    try:
        sd.wrap_trie(code, raw)
    except ValueError:
        pass
    else:
        raise AssertionError("An invalid SD trie was packaged")
print("Nine legacy and fifteen SD packs, RC ZIP, checksums, notices and flash installation passed")
