#!/usr/bin/env python3
"""Validate an Inky download through production I18n and the real simulator picker.

The output directory retains logs, framebuffer captures, settings, and flash state.
Use the exact export produced by Inky; this harness never rewrites its contents.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CRASH_PATTERNS = ("Assertion failed", "Segmentation fault", "std::bad_alloc",
                  "AddressSanitizer", "UndefinedBehaviorSanitizer")


def parse_source(path: Path) -> dict[str, str]:
    values = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = re.fullmatch(r'([A-Za-z_][A-Za-z0-9_]*)\s*:\s*("(?:[^"\\]|\\["\\nt])*")\s*(?:#.*)?', line)
        assert match, (path, number, line)
        key, quoted = match.groups()
        assert key not in values, (path, number, "duplicate", key)
        values[key] = json.loads(quoted)
    return values


def checked_run(command, *, env, cwd):
    result = subprocess.run(list(map(str, command)), env=env, cwd=cwd,
                            text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


def test_host(args, values):
    work = args.output / "host"
    languages = work / "fs_/.crossink/languages"
    languages.mkdir(parents=True)
    shutil.copyfile(args.export, languages / "inky.yaml")
    env = dict(os.environ, CROSSINK_TEST_SD=str(work / "fs_"),
               CROSSINK_LANGUAGE_FLASH=str(work / "flash.bin"))
    run = lambda *command: checked_run([args.installer, *command], env=env, cwd=work)
    assert run("boot", values["_language_code"]).startswith("EN|English|Settings|")
    assert not (work / "flash.bin").exists(), "Boot must not provision flash"
    installed = run("install", "EN", "0", "/.crossink/languages/inky.yaml")
    code, generation = installed.split("|")
    assert code == values["_language_code"].upper() and generation == "1", installed
    expected_name = values["_language_name"]
    english = parse_source(ROOT / "lib/I18n/translations/english.yaml")
    title = values.get("STR_SETTINGS_TITLE") or english["STR_SETTINGS_TITLE"]
    boot = run("boot", code, generation)
    assert boot.startswith(f"{code}|{expected_name}|{title}|"), boot
    # Look up every exported value that survives the real build's unused-key strip.
    # No independent parser/cache implementation can mask a serialization mismatch.
    checks = 0
    for key, value in values.items():
        if not key.startswith("STR_"):
            continue
        result = subprocess.run([str(args.installer), "lookup", code, generation, key],
                                env=env, cwd=work, text=True, capture_output=True, timeout=5)
        if result.returncode == 2:  # known canonical keys stripped from this firmware
            assert key in english, key
            continue
        assert result.returncode == 0, (key, result.stderr)
        assert result.stdout == (value or english[key]), (key, value, result.stdout)
        checks += 1
    # Deliberately omitted entries must resolve to the canonical English fallback.
    omitted = next((key for key in english if key.startswith("STR_") and key not in values
                    and subprocess.run([str(args.installer), "lookup", code, generation, key],
                                       env=env, cwd=work, capture_output=True).returncode == 0), None)
    if omitted:
        assert run("lookup", code, generation, omitted) == english[omitted]
    (languages / "inky.yaml").unlink()
    assert run("boot", code, generation) == boot, "Cached language must work without SD source"
    (args.output / "host-result.json").write_text(json.dumps({
        "code": code, "generation": int(generation), "exported_runtime_values_checked": checks,
        "missing_key_fallback_checked": omitted, "boot": boot}, indent=2) + "\n")
    print(f"PASS production install/lookup/boot: {code}, {checks} exported runtime values", flush=True)


def test_simulator(args, values):
    work = args.output / "simulator"
    state = work / "fs_/.crosspoint"
    state.mkdir(parents=True)
    languages = work / "fs_/.crossink/languages"
    languages.mkdir(parents=True)
    shutil.copyfile(args.export, languages / "inky.yaml")
    books = work / "fs_/books"
    books.mkdir()
    shutil.copyfile(args.book, books / "test.epub")
    settings = state / "crossink-settings.json"
    settings.write_text(json.dumps({"language": "EN", "uiTheme": 0}))
    # Same physical button route as the existing language-picker regression.
    actions = ["UP", "ENTER", "ENTER", "ENTER", "ENTER", "DOWN", "ENTER",
               "DOWN", "DOWN", "DOWN", "ENTER", "DOWN", "ENTER"]
    before = ";".join(f"{1000 + i * 500}:{key}" for i, key in enumerate(actions))
    # After the installation's real ESP.restart(): Home -> Browse -> books -> EPUB.
    after = "1000:ENTER;1800:ENTER;2600:ENTER;9000:DOWN;11000:QUIT"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy",
               CROSSINK_LANGUAGE_FLASH=str(work / "flash.bin"),
               CROSSPOINT_SIM_INPUT_SCRIPT=before + ";22000:QUIT",
               CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_REBOOT=after,
               CROSSPOINT_SIM_SCREENSHOTS=f"6200:{args.output / 'language-picker.bmp'}",
               CROSSPOINT_SIM_SCREENSHOTS_AFTER_REBOOT=(
                   f"700:{args.output / 'translated-home.bmp'};"
                   f"8000:{args.output / 'epub-reader.bmp'};"
                   f"9800:{args.output / 'epub-next-page.bmp'}"))
    result = subprocess.run([str(args.program)], env=env, cwd=work, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    (args.output / "simulator-ui.log").write_text(result.stdout)
    assert result.returncode == 0, (result.returncode, result.stdout[-4000:])
    assert not any(pattern in result.stdout for pattern in CRASH_PATTERNS), result.stdout[-4000:]
    saved = json.loads(settings.read_text())
    code = values["_language_code"].upper()
    assert saved["language"] == code and saved["languageCacheGeneration"] == 1, saved
    assert f"Mapped {code}" in result.stdout, result.stdout[-4000:]
    assert result.stdout.count("Entering activity: Home") >= 2, result.stdout[-4000:]
    assert "Entering activity: EpubReader" in result.stdout, result.stdout[-4000:]
    assert "Loading file:" in result.stdout, result.stdout[-4000:]
    for capture in ("language-picker.bmp", "translated-home.bmp", "epub-reader.bmp", "epub-next-page.bmp"):
        assert (args.output / capture).stat().st_size > 1000, capture
    assert (args.output / "epub-reader.bmp").read_bytes() != (args.output / "epub-next-page.bmp").read_bytes(), "Page-turn capture must change"
    print(f"PASS real simulator language picker, reboot, EPUB open/page turn: {code}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--book", type=Path, default=ROOT / "test/epubs/test_reader_rendering_matrix.epub")
    parser.add_argument("--output", type=Path, required=True,
                        help="New, empty output directory (all evidence is retained)")
    args = parser.parse_args()
    for key in ("export", "catalog", "installer", "program", "book", "output"):
        setattr(args, key, getattr(args, key).resolve())
    assert not args.output.exists(), f"Refusing to mix runs in {args.output}"
    args.output.mkdir(parents=True)
    values = parse_source(args.export)
    catalog = json.loads(args.catalog.read_text())
    english_path = ROOT / "lib/I18n/translations/english.yaml"
    english = parse_source(english_path)
    assert catalog["englishSourceSha256"] == hashlib.sha256(english_path.read_bytes()).hexdigest()
    assert {entry["key"]: entry["english"] for entry in catalog["entries"]} == {
        key: value for key, value in english.items() if key.startswith("STR_")}
    assert values["_template_source_sha256"] == catalog["englishSourceSha256"]
    assert values["_template_keyset_sha256"] == catalog["keysetSha256"]
    assert values["_source_commit"] == catalog["sourceCommit"]
    assert values["_firmware_version"] == catalog["firmwareVersion"]
    assert all(key in english for key in values if key.startswith("STR_"))
    shutil.copyfile(args.export, args.output / "inky-export.yaml")
    shutil.copyfile(args.catalog, args.output / "language-template.json")
    assert values["_language_code"].upper() != "EN", "Use a synthetic non-English identity"
    test_host(args, values)
    test_simulator(args, values)


if __name__ == "__main__":
    main()
