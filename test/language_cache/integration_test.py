#!/usr/bin/env python3
"""Exercise production I18n, its flash HAL, and generated data across fresh processes."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary, root = sys.argv[1], Path(sys.argv[2]).resolve()
sys.path.insert(0, str(root / "scripts"))
from generate_language_template import generate
with tempfile.TemporaryDirectory(prefix="crossink-language-") as temporary:
    base = Path(temporary)
    languages = base / "sd/.crossink/languages"
    languages.mkdir(parents=True)
    env = dict(os.environ, CROSSINK_TEST_SD=str(base / "sd"), CROSSINK_LANGUAGE_FLASH=str(base / "flash.bin"))

    def run(*args):
        result = subprocess.run([binary, *map(str, args)], env=env, text=True, capture_output=True)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return result.stdout.strip()

    # CrossPoint's namespace must never be discovered as a CrossInk source.
    foreign = base / "sd/.crosspoint/languages"
    foreign.mkdir(parents=True)
    (foreign / "foreign.yaml").write_text(
        '_language_code: "ZZ-FOREIGN"\n_language_name: "CrossPoint language"\n')
    assert "ZZ-FOREIGN|" not in run("scan")

    assert run("boot", "ZZ-CUSTOM").startswith("EN|English|Settings|")
    assert not (base / "flash.bin").exists()  # boot never provisions or scans SD
    # The full online source validator uses the live generated contract, keeps
    # active strings pinned and never creates a flash image for a download.
    artifacts = generate("native-fixture")
    contract = json.loads(artifacts["language-template.json"])
    french = next(item for item in contract["languages"] if item["code"] == "FR")
    online = languages / "FR.yaml"
    source = artifacts[french["translation"]["filename"]]
    online.write_bytes(source)
    run("validate", "FR", 0, "/.crossink/languages/FR.yaml")
    assert not (base / "flash.bin").exists()
    online.write_bytes(source.replace(contract["keysetSha256"].encode(), b"0" * 64))
    rejected = subprocess.run([binary, "validate", "FR", "0", "/.crossink/languages/FR.yaml"],
                              env=env, capture_output=True)
    assert rejected.returncode == 5
    assert not (base / "flash.bin").exists()
    online.unlink()
    sample = languages / "custom.yaml"
    sample.write_text('_language_code: "ZZ-CUSTOM"\n_language_name: "My language"\n_direction: "rtl"\n'
                      '_keyboard: "HE"\nSTR_SETTINGS_TITLE: "Custom settings"\n', encoding="utf-8")
    # The reported factory layout uses an SD cache, leaving every flash byte
    # untouched. Cached strings survive source removal and never read SD in get().
    factory_flash = b"\xa5" * 0x14000
    (base / "flash.bin").write_bytes(factory_flash)
    assert run("boot", "ZZ-CUSTOM").startswith("EN|English|Settings|")
    sd_cache = base / "sd/.crosspoint/asset-cache.bin"
    assert not sd_cache.exists()
    env["CROSSINK_TEST_BATCHED_INSTALL"] = "1"
    # Use a real multi-string language so per-string syncs would exceed the cap.
    sample.write_text(sample.read_text() + 'STR_BACK: "Retour"\nSTR_CANCEL: "Annuler"\n')
    assert run("install", "EN", 0, "/.crossink/languages/custom.yaml") == "ZZ-CUSTOM|1"
    del env["CROSSINK_TEST_BATCHED_INSTALL"]
    assert (base / "flash.bin").read_bytes() == factory_flash
    assert run("boot", "ZZ-CUSTOM", 1) == "ZZ-CUSTOM|My language|Custom settings|1"
    original_source = sample.read_text()
    sample.unlink()
    assert run("boot", "ZZ-CUSTOM", 1) == "ZZ-CUSTOM|My language|Custom settings|1"
    sample.write_text(original_source)
    old_sd = sd_cache.read_bytes()
    run("unchanged", "ZZ-CUSTOM", 1, "/.crossink/languages/custom.yaml")
    assert sd_cache.read_bytes() == old_sd
    # Inject a failure after each SD flush: erase, bulk image, publication.
    # The settings-pinned old slot and its UI stay usable even when the new
    # marker reached disk but its sync reported failure.
    sample.write_text(original_source.replace("Custom settings", "Replacement settings"))
    for failure in (1, 2, 3):
        env["CROSSINK_TEST_FAIL_SYNC"] = str(failure)
        rejected = subprocess.run([binary, "install", "ZZ-CUSTOM", "1", "/.crossink/languages/custom.yaml"],
                                  env=env, capture_output=True)
        assert rejected.returncode == 5, (failure, rejected.stdout, rejected.stderr)
        del env["CROSSINK_TEST_FAIL_SYNC"]
        assert sd_cache.read_bytes()[:-0x10000] == old_sd[:-0x10000]
        assert run("boot", "ZZ-CUSTOM", 1) == "ZZ-CUSTOM|My language|Custom settings|1"
        sd_cache.write_bytes(old_sd)
    sample.write_text(original_source)
    # A missing cache falls back to English; corrupt/truncated images are not
    # overwritten by a failed install.
    sd_cache.write_bytes(b"truncated")
    assert run("boot", "ZZ-CUSTOM", 1).startswith("EN|English|Settings|")
    rejected = subprocess.run([binary, "install", "EN", "0", "/.crossink/languages/custom.yaml"],
                              env=env, capture_output=True)
    assert rejected.returncode == 5 and sd_cache.read_bytes() == b"truncated"
    sd_cache.unlink()
    assert run("boot", "ZZ-CUSTOM", 1).startswith("EN|English|Settings|")
    assert not sd_cache.exists()
    assert (base / "flash.bin").read_bytes() == factory_flash
    # Legacy filesystem data must not block installation through the real HAL.
    # The preceding data region and unselected slot stay untouched.
    old_flash = bytearray(b"\xff" * 0x360000)
    old_flash[8] = 0x42
    old_flash[-0x20000:] = b"\xa5" * 0x20000
    (base / "flash.bin").write_bytes(old_flash)
    assert run("boot", "ZZ-CUSTOM").startswith("EN|English|Settings|")
    assert (base / "flash.bin").read_bytes() == old_flash
    assert "ZZ-CUSTOM|My language|0|" in run("scan")
    assert run("install", "EN", 0, "/.crossink/languages/custom.yaml") == "ZZ-CUSTOM|1"
    installed_flash = (base / "flash.bin").read_bytes()
    assert installed_flash[:-0x20000] == old_flash[:-0x20000]
    assert installed_flash[-0x10000:] == old_flash[-0x10000:]
    assert run("boot", "ZZ-CUSTOM", 1) == "ZZ-CUSTOM|My language|Custom settings|1"
    run("unchanged", "ZZ-CUSTOM", 1, "/.crossink/languages/custom.yaml")
    assert (base / "flash.bin").read_bytes() == installed_flash
    unchanged_sample = languages / "same.yaml"
    unchanged_sample.write_text("# Formatting-only edit\n" + sample.read_text())
    sample.write_text(sample.read_text().replace("Custom settings", "Revised settings"))
    assert "|Custom settings|" in run("boot", "ZZ-CUSTOM", 1)  # edits require apply
    run("unchanged", "ZZ-CUSTOM", 1, "/.crossink/languages/same.yaml", "/.crossink/languages/custom.yaml")
    unchanged_sample.unlink()
    assert "|Custom settings|" in run("boot", "ZZ-CUSTOM", 1)  # failed selection save keeps old generation
    assert "|Revised settings|" in run("boot", "ZZ-CUSTOM", 2)
    sample.unlink()
    assert "|Revised settings|" in run("boot", "ZZ-CUSTOM", 2)
    assert "ZZ-CUSTOM|My language|0|" in run("scan", "ZZ-CUSTOM", 2)  # cached option without SD source
    assert run("boot", "ZZ-CUSTOM", 999).startswith("EN|English|Settings|")
    assert run("boot", "EN").startswith("EN|English|Settings|")

    # Every inherited printf contract must reject incompatible SD translations.
    contracts = {
        "STR_HOLD_FOR_KEYBOARD": ("%u", "Hold %s for Keyboard"),
        "STR_LIBRARY_SCAN_COUNT": ("%s", "Reading your books... %u"),
        "STR_LIBRARY_FILES_COUNT": ("%s", "%u files"),
        "STR_FOLDER_SYNC_COUNTS": ("%s", "Synced: %u · Skipped: %u · Failed: %u"),
    }
    sample.write_text('_language_code: "TEST-FMT"\n_language_name: "Unsafe format fixture"\n' +
                      ''.join(f'{key}: "{bad}"\n' for key, (bad, _) in contracts.items()))
    fmt_code, fmt_generation = run("install", "ZZ-CUSTOM", 2, "/.crossink/languages/custom.yaml").split("|")
    for key, (_, expected) in contracts.items():
        assert run("lookup", fmt_code, fmt_generation, key) == expected
    sample.unlink()

    # Install every existing starter file using the actual firmware schema.
    code, generation = "ZZ-CUSTOM", 2
    for path in sorted((root / "lib/I18n/translations").glob("*.yaml")):
        if path.name == "english.yaml":
            continue
        shutil.copy2(path, languages / path.name)
        installed = run("install", code, generation, f"/.crossink/languages/{path.name}")
        code, generation = installed.split("|")
        assert run("boot", code, generation).startswith(code + "|")
    # Duplicate identities remain visible and disabled, not arbitrarily selected.
    shutil.copy2(languages / "french.yaml", languages / "french-copy.yaml")
    assert any(line.startswith("FR|") and "|1|" in line for line in run("scan").splitlines())
    result = subprocess.run([binary, "scan"], env=dict(env, CROSSINK_TEST_DIRECTORY_OOM="1"),
                            text=True, capture_output=True)
    assert result.returncode == 5 and "out of memory" in result.stderr, result
    # All starter files fit the fixed arena together. Long user-controlled
    # names/filenames and excessive file counts fail without aborting or clipping.
    for path in languages.iterdir():
        path.unlink()
    for i in range(16):
        (languages / (f"long-{i}-" + "x" * 90 + ".yaml")).write_text(
            f'_language_code: "LONG-{i}"\n_language_name: "' + "x" * 90 + '"\nSTR_SETTINGS_TITLE: "Test"\n')
    result = subprocess.run([binary, "scan"], env=env, text=True, capture_output=True)
    assert result.returncode == 5 and "exceeds limits" in result.stderr, result
    for path in languages.iterdir():
        path.unlink()
    for i in range(65):
        (languages / f"{i}.yaml").write_text(f'_language_code: "X{i}"\n_language_name: "X{i}"\n')
    result = subprocess.run([binary, "scan"], env=env, text=True, capture_output=True)
    assert result.returncode == 5 and "exceeds limits" in result.stderr, result
print("I18n integration passed: stable pointers, restart/apply, missing SD files, fallback, and all starter languages")
