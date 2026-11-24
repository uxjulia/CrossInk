#!/usr/bin/env python3
"""Verify both exporters and legacy promotion without touching remote storage."""

import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

root = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / "scripts"))
import export_github_language_assets as github
import package_hyphenation_v2 as packs
import publish_language_assets as publisher


def catalogs(channel):
    publisher.args = SimpleNamespace(channel=channel)
    legacy, legacy_files = publisher.build_hyphenation()
    modern, modern_files = publisher.build_hyphenation(extended=True)
    for catalog, files in ((legacy, legacy_files), (modern, modern_files)):
        publisher.validate_companions(catalog, files)
    assert publisher.catalog_key(legacy) == f"catalogs/{channel}/hyphenation.json"
    assert publisher.catalog_key(modern) == f"catalogs/{channel}/hyphenation-v2.json"
    assert legacy["schema"] == 1 and modern["schema"] == 2
    assert legacy["format"] == modern["format"] == 1
    assert legacy["revision"] != modern["revision"]
    modern_rows = {e["code"]: e for e in modern["entries"]}
    assert set(modern_rows) == set(packs.CODES)
    assert len(legacy["entries"]) == 9
    assert all(e == modern_rows[e["code"]] for e in legacy["entries"])
    dutch = modern_rows["nl"]
    prefix = f"snapshots/{modern['revision']}/"
    assert modern_files[prefix + f"hyphenation/{dutch['sha256']}.cphyph"] == packs.dutch_pack(root)
    assert dutch["bytes"] == 64313
    notice = json.loads(modern_files[prefix + "notices.json"])["hyphenation"]["nl"]
    assert notice["license"] == "MIT"
    assert {Path(ref["path"]).name for ref in notice["files"]} == {"LICENSE-MIT", "README.md", "hyph-nl.tex"}
    supplement, supplemental_files = publisher.build_supplemental_hyphenation()
    publisher.validate_companions(supplement, supplemental_files)
    assert publisher.catalog_key(supplement) == f"catalogs/{channel}/hyphenation-supplement.json"
    assert supplement["schema"] == 3 and supplement["format"] == 1
    assert len(supplement["entries"]) == 18 and not set(modern_rows) & {e["code"] for e in supplement["entries"]}
    finnish, finnish_files = publisher.build_supplemental_hyphenation(finnish_only=True)
    publisher.validate_companions(finnish, finnish_files)
    assert publisher.catalog_key(finnish) == f"catalogs/{channel}/hyphenation/fi.json"
    assert finnish["schema"] == finnish["format"] == 1
    assert finnish["entries"] == [e for e in supplement["entries"] if e["code"] == "fi"]
    payload = f"hyphenation/{finnish['entries'][0]['sha256']}.cphyph"
    assert finnish_files[f"snapshots/{finnish['revision']}/{payload}"] == (
        supplemental_files[f"snapshots/{supplement['revision']}/{payload}"])
    return [(legacy, legacy_files), (modern, modern_files), (supplement, supplemental_files), (finnish, finnish_files)]


for channel in ("rc", "stable"):
    bundles = catalogs(channel)
    keys = [publisher.catalog_key(catalog) for catalog, _ in bundles]
    files = github.github_files(bundles[:2])
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory)
        legacy_pointer = output / keys[0]
        legacy_pointer.parent.mkdir(parents=True)
        original = publisher.encode(bundles[0][0]) + b"\n"  # A published pointer may differ byte-for-byte.
        legacy_pointer.write_bytes(original)
        github.write_files(output, files)
        assert legacy_pointer.read_bytes() == original
        assert (output / keys[1]).read_bytes() == publisher.encode(bundles[1][0])
        for (catalog, _), key in zip(bundles[:2], keys[:2], strict=True):
            assert (output / f"snapshots/{catalog['revision']}" / key).read_bytes() == files[key]

    # Exercise the real S3 planning code with a fake public bucket. Existing
    # legacy pointer bytes must not be promoted or needed by the new snapshot.
    with tempfile.TemporaryDirectory() as directory:
        fake_root = Path(directory)
        (fake_root / "platformio.ini").write_text("version = 2.1.0\n")
        remote = {keys[0]: original}
        published = []

        def get(key, _destination, missing_ok=False):
            if key in remote:
                return remote[key], '"etag"'
            assert missing_ok, key
            return None, None

        def put(*args):
            assert args[0] == "put-object"
            key = args[args.index("--key") + 1].removeprefix("v1/")
            assert key != keys[0], "legacy pointer was about to be rewritten"
            remote[key] = Path(args[args.index("--body") + 1]).read_bytes()
            published.append(key)

        with patch.object(publisher, "root", fake_root), \
             patch.object(publisher, "build_hyphenation", side_effect=bundles[:2]), \
             patch.object(publisher, "build_supplemental_hyphenation", side_effect=bundles[2:]), \
             patch.object(publisher, "remote_get", side_effect=get), \
             patch.object(publisher, "aws", side_effect=put), \
             patch.object(publisher.subprocess, "check_output", return_value="a" * 40 + "\n"), \
             patch.object(sys, "argv", ["publish_language_assets", "--publish", "--assets", "hyphenation",
                                        "--channel", channel]):
            publisher.main()
        assert remote[keys[0]] == original
        assert published[-3:] == keys[1:]
        legacy_prefix = f"snapshots/{bundles[0][0]['revision']}/"
        assert not any(key.startswith(legacy_prefix) for key in published)
