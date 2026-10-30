#!/usr/bin/env python3
"""Build and run the simulator smoke test against an isolated fs_ directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)
THEMES = {
    "classic": 0,
    "lyra": 1,
    "lyra-extended": 2,
    "lyra_extended": 2,
    "lyra3": 2,
    "lyra-3-covers": 2,
    "roundedraff": 3,
    "rounded-raff": 3,
    "lyra-carousel": 4,
    "lyra_carousel": 4,
    "carousel": 4,
    "dashboard": 6,
    "cover-grid": 7,
}


def program_path(env_name: str) -> Path:
    build_dir = Path(os.environ.get("PLATFORMIO_BUILD_DIR", ROOT / ".pio" / "build"))
    return build_dir / env_name / "program"


def build_simulator(env_name: str) -> None:
    print(f"Building {env_name} simulator...", flush=True)
    proc = subprocess.run(["pio", "run", "-e", env_name, "-j1"], cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def prepare_fs(temp_root: Path, book: Path) -> str:
    books_dir = temp_root / "fs_" / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    target = books_dir / book.name
    shutil.copy2(book, target)
    return f"/books/{book.name}"


def prepare_asset_mock(temp_root: Path, snapshot: str | None, hyphenation_only: bool = False) -> Path:
    """Copy immutable producer bytes into a disposable mock, or make local fixtures.

    Preserve full snapshot paths for companions and provide scoped translation
    discovery for the running firmware. Never mutate
    the supplied snapshot and never qualify TLS/publication with this transport.
    """
    mock = temp_root / "asset-http-mock"
    mock.mkdir()
    if snapshot:
        source = Path(snapshot).resolve()
        if source.is_file():
            source = source.parent
        if not (source / "snapshot.json").is_file():
            raise ValueError("--asset-snapshot must name one snapshot directory or snapshot.json")
        kinds = (("hyphenation", "cphyph"),) if hyphenation_only else (("hyphenation", "cphyph"), ("translation", "yaml"))
        for kind, extension in kinds:
            catalog_file = source / "catalogs" / "stable" / f"{kind}.json"
            catalog = json.loads(catalog_file.read_bytes())
            for entry in catalog["entries"]:
                if not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) or not 0 < entry["bytes"] <= 262144:
                    raise ValueError("Snapshot contains an invalid digest or payload size")
                payload = source / kind / f"{entry['sha256']}.{extension}"
                data = payload.read_bytes()
                if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                    raise ValueError(f"Snapshot payload mismatch: {payload}")
                destination = mock / "snapshots" / catalog["revision"] / kind / payload.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(payload, destination)
            shutil.copy2(catalog_file, mock / catalog_file.name)
        # Derive each recipient closure from both exact indexes, including
        # opaque source archives. This fixture validator never approves a release.
        indexes = {}
        for name in ("notices.json", "sources.json"):
            raw = (source / name).read_bytes()
            if not 0 < len(raw) <= 2097152:
                raise ValueError("Companion index exceeds bound")
            indexes[name] = json.loads(raw)
        for kind, _ in kinds:
            catalog = json.loads((mock / f"{kind}.json").read_bytes())
            prefix = f"snapshots/{catalog['revision']}/"
            for name in indexes:
                target = mock / prefix / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source / name, target)
            for entry in catalog["entries"]:
                references = {}
                total = 0
                for index in indexes.values():
                    files = index[kind][entry["code"]]["files"]
                    if not 0 < len(files) <= 16:
                        raise ValueError("Selected companion list exceeds bound")
                    for ref in files:
                        path = ref["path"]
                        if (len(path) > 240 or not path.startswith(prefix + "source/") or
                            not re.fullmatch(r"[A-Za-z0-9._/-]+", path) or
                            any(part in ("", ".", "..") for part in path.split("/")) or
                            not re.fullmatch(r"[0-9a-f]{64}", ref["sha256"]) or
                            not 0 < ref["bytes"] <= 2097152):
                            raise ValueError("Invalid companion reference")
                        total += ref["bytes"]
                        if path in references and references[path] != ref:
                            raise ValueError("Conflicting companion references")
                        references[path] = ref
                if len(references) > 32 or total > 8388608:
                    raise ValueError("Companion closure exceeds bound")
                expected = temp_root / "asset-expected" / f"{kind}-{entry['code']}.txt"
                expected.parent.mkdir(exist_ok=True)
                lines = []
                for path, ref in references.items():
                    original = source / path.removeprefix(prefix)
                    data = original.read_bytes()
                    if len(data) != ref["bytes"] or hashlib.sha256(data).hexdigest() != ref["sha256"]:
                        raise ValueError(f"Companion mismatch: {original}")
                    target = mock / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(original, target)
                    lines.append(path.removeprefix(prefix))
                expected.write_text("\n".join([*lines, "notices.json", "sources.json"]) + "\n")
        print(f"Asset smoke uses copied, unpublished snapshot and complete indexed companions: {source}", flush=True)
    else:
        from generate_language_template import generate
        from package_hyphenation import CODES, pack_bytes
        generated = generate("simulator-local-fixture")
        contract = json.loads(generated["language-template.json"])
        common = {"schema": 1, "channel": "stable", "revision": contract["sourceCommit"],
                  "format": 1, "requires": 0, "notices": "notices.json", "sources": "sources.json"}
        hyphenation = dict(common, kind="hyphenation", entries=[])
        for code in CODES:
            data = pack_bytes(code)
            digest = hashlib.sha256(data).hexdigest()
            (mock / f"{digest}.cphyph").write_bytes(data)
            hyphenation["entries"].append({"code": code, "bytes": len(data), "sha256": digest})
        translation = dict(common, kind="translation", source=contract["englishSourceSha256"],
                           keyset=contract["keysetSha256"], entries=[])
        for language in contract["languages"]:
            if "translation" not in language:
                continue
            reference = language["translation"]
            data = generated[reference["filename"]]
            (mock / f"{reference['sha256']}.yaml").write_bytes(data)
            translation["entries"].append({"code": language["code"], "name": language["name"],
                                            "bytes": len(data), "sha256": reference["sha256"]})
        for kind, catalog in (("hyphenation", hyphenation), ("translation", translation)):
            (mock / f"{kind}.json").write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")))
        revision = common["revision"]
        prefix = f"snapshots/{revision}/"
        notice = b"TEST-ONLY functional fixture; not a distributable source/license arrangement.\n"
        path = prefix + "source/TEST-ONLY.txt"
        target = mock / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(notice)
        ref = {"path": path, "bytes": len(notice), "sha256": hashlib.sha256(notice).hexdigest()}
        index = {kind: {entry["code"]: {"files": [ref]} for entry in catalog["entries"]}
                 for kind, catalog in (("hyphenation", hyphenation), ("translation", translation))}
        for name in ("notices.json", "sources.json"):
            (mock / prefix / name).write_text(json.dumps(index, separators=(",", ":")))
        for kind, catalog in (("hyphenation", hyphenation), ("translation", translation)):
            extension = "cphyph" if kind == "hyphenation" else "yaml"
            for entry in catalog["entries"]:
                payload = mock / f"{entry['sha256']}.{extension}"
                destination = mock / prefix / kind / payload.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(payload, destination)
                expected = temp_root / "asset-expected" / f"{kind}-{entry['code']}.txt"
                expected.parent.mkdir(exist_ok=True)
                expected.write_text("source/TEST-ONLY.txt\nnotices.json\nsources.json\n")
        print("Asset smoke uses disposable working-source fixtures, not a release snapshot", flush=True)
    translation_file = mock / "translation.json"
    if translation_file.is_file():
        from generate_language_template import generate
        from publish_language_assets import catalog_key
        contract = json.loads(generate("simulator-local-fixture")["language-template.json"])
        # Obsolete-snapshot tests intentionally serve mismatched bytes at the
        # running firmware's lookup path, so parser rejection is still exercised.
        lookup = dict(json.loads(translation_file.read_bytes()),
                      source=contract["englishSourceSha256"], keyset=contract["keysetSha256"])
        destination = mock / catalog_key(lookup)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(translation_file, destination)
    return mock


def run_smoke(args: argparse.Namespace) -> int:
    book = Path(args.book).resolve()
    if not book.exists():
        print(f"Smoke test book not found: {book}", file=sys.stderr)
        return 2

    if args.build:
        build_simulator(args.env)

    program = program_path(args.env)
    if not program.exists():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        print(f"Run: pio run -e {args.env}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="crossink-sim-smoke-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        simulator_book_path = prepare_fs(temp_root, book)

        if args.font_dir:
            shutil.copytree(Path(args.font_dir), temp_root / "fs_" / "fonts", dirs_exist_ok=True)
        env = os.environ.copy()
        factory_flash = None
        if args.factory_flash:
            factory_flash = temp_root / "language-flash.bin"
            factory_flash.write_bytes(b"\xa5" * 0x14000)
            env["CROSSINK_LANGUAGE_FLASH"] = str(factory_flash)
        if args.asset_downloads:
            env["CROSSINK_SIMULATOR_SMOKE_ASSET_DOWNLOADS"] = "1"
            env["CROSSINK_ASSET_SMOKE_STAGE"] = "0"
            env.pop("CROSSINK_ASSET_SMOKE_HYPHENATION_ONLY", None)
            env.pop("CROSSINK_ASSET_SMOKE_EXPECT_TRANSLATION_REJECTION", None)
            env.pop("CROSSINK_ASSET_SMOKE_EMPTY_CATALOG", None)
            env.pop("CROSSINK_ASSET_SMOKE_SAVE_FAILURE", None)
            if args.asset_save_failure:
                env["CROSSINK_ASSET_SMOKE_SAVE_FAILURE"] = "1"
            if args.asset_expect_translation_rejection:
                env["CROSSINK_ASSET_SMOKE_EXPECT_TRANSLATION_REJECTION"] = "1"
            env["CROSSINK_SIM_HTTP_MOCK_ROOT"] = str(prepare_asset_mock(temp_root, args.asset_snapshot, args.asset_hyphenation_only))
            if args.asset_empty_catalog:
                catalog_path = Path(env["CROSSINK_SIM_HTTP_MOCK_ROOT"]) / "hyphenation.json"
                catalog = json.loads(catalog_path.read_bytes())
                catalog["entries"] = []
                catalog_path.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")))
                env["CROSSINK_ASSET_SMOKE_EMPTY_CATALOG"] = "1"
            env["CROSSPOINT_SIM_WIFI_NETWORKS"] = "asset-smoke:-40:open"
            env["CROSSINK_LANGUAGE_FLASH"] = str(temp_root / "language-flash.bin")
            if args.asset_hyphenation_only:
                env["CROSSINK_ASSET_SMOKE_HYPHENATION_ONLY"] = "1"
            if args.asset_captures:
                env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.asset_captures).resolve())
        if args.language_file:
            language_path = temp_root / "fs_" / ".crosspoint" / "languages" / "smoke.yaml"
            language_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(args.language_file), language_path)
            env["CROSSINK_LANGUAGE_FLASH"] = str(temp_root / "language-flash.bin")
            installer_env = dict(env, CROSSINK_TEST_SD=str(temp_root / "fs_"))
            installed = subprocess.run(
                [str(Path(args.language_installer).resolve()), "install", "EN", "0",
                 "/.crosspoint/languages/smoke.yaml"],
                cwd=temp_root, env=installer_env, text=True, capture_output=True, check=True, timeout=30,
            )
            code, generation = installed.stdout.strip().split("|")
            (temp_root / "fs_" / ".crosspoint" / "crossink-settings.json").write_text(
                json.dumps({"language": code, "languageCacheGeneration": int(generation)})
            )
        if args.hyphenation:
            from package_hyphenation import pack_bytes
            # Only the disposable fixture changes: exercise the reader's book
            # language independently of the selected interface language.
            target = temp_root / "fs_" / simulator_book_path.lstrip("/")
            updated = target.with_suffix(".hyphenation.epub")
            languages = 0
            with zipfile.ZipFile(target) as source, zipfile.ZipFile(updated, "w") as destination:
                for entry in source.infolist():
                    data = source.read(entry)
                    if entry.filename.endswith(".opf"):
                        data, found = re.subn(rb"(<dc:language[^>]*>).*?(</dc:language>)",
                                             rb"\g<1>de-DE\g<2>", data, flags=re.S)
                        languages += found
                    destination.writestr(entry, data)
            if not languages:
                raise ValueError("Hyphenation smoke fixture needs dc:language metadata")
            updated.replace(target)
            packs = temp_root / "fs_" / ".crosspoint" / "hyphenation"
            packs.mkdir(parents=True, exist_ok=True)
            data = pack_bytes("de")
            (packs / "hyph-de.cphyph").write_bytes(data)
            (packs / "hyph-fr.cphyph").write_bytes(data)  # mismatched header identity
            env["CROSSINK_SIMULATOR_SMOKE_HYPHENATION"] = "1"
            env["CROSSINK_HYPHENATION_SMOKE_UI"] = code if args.language_file else "EN"
        if args.hyphenation_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.hyphenation_captures).resolve())
        if args.filename_font_dir:
            shutil.copytree(Path(args.filename_font_dir), temp_root / "fs_" / ".crosspoint" / "languages" / "fonts", dirs_exist_ok=True)
        if args.filename_font_family:
            env["CROSSINK_SIMULATOR_SMOKE_FILENAME_FONT"] = args.filename_font_family
        if args.filename_font_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.filename_font_captures).resolve())
        if args.screen_calibration:
            env["CROSSINK_SIMULATOR_SMOKE_CALIBRATION"] = "1"
            env["CROSSINK_CALIBRATION_PATH"] = str(temp_root / "device-calibration.nvs")
        if args.status_bar_lifecycle:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_LIFECYCLE"] = "1"
        if args.chapter_percentage:
            env["CROSSINK_SIMULATOR_SMOKE_CHAPTER_PERCENTAGE"] = "1"
        if args.centered_status_bar:
            env["CROSSINK_SIMULATOR_SMOKE_CENTERED_STATUS_BAR"] = "1"
        if args.status_bar_feature:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_FEATURE"] = "1"
        if args.status_bar_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.status_bar_captures).resolve())
        if args.clipping_selection:
            env["CROSSINK_SIMULATOR_SMOKE_CLIPPING_SELECTION"] = "1"
        if args.dictionary or args.clipping_selection:
            dictionary = temp_root / "fs_" / "dictionary-smoke"
            dictionary.mkdir()
            words = sorted(["accord", "aime", "après", "d'accord", "est", "histoire", "inspecteur", "école"])
            index = bytearray()
            definitions = bytearray()
            for word in words:
                definition = f"Definition of {word}".encode()
                index.extend(word.encode() + b"\0" + struct.pack(">II", len(definitions), len(definition)))
                definitions.extend(definition)
            (dictionary / "dict.idx").write_bytes(index)
            (dictionary / "dict.dict").write_bytes(definitions)
            (dictionary / "dict.ifo").write_text(
                "StarDict's dict ifo file\nversion=2.4.2\nbookname=French smoke\n"
                f"wordcount={len(words)}\nidxfilesize={len(index)}\nsametypesequence=m\n"
            )
            if args.dictionary and not args.clipping_selection:
                env["CROSSINK_SIMULATOR_SMOKE_DICTIONARY"] = "1"
        if args.font_dir and args.font_family:
            env["CROSSINK_SIMULATOR_SMOKE_ISOLATED_FONTS"] = "1"
        if args.font_family:
            env["CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY"] = args.font_family
        if args.reader_font_layout:
            env["CROSSINK_SIMULATOR_SMOKE_READER_FONT_LAYOUT"] = "1"
        if args.c3_font_sizes:
            env["CROSSINK_SIMULATOR_SMOKE_C3_FONT_SIZES"] = "1"
        if args.font_size_book:
            env["CROSSINK_SIMULATOR_SMOKE_FONT_SIZE_BOOK"] = "1"
        if args.gesture_shortcuts:
            env["CROSSINK_SIMULATOR_SMOKE_GESTURE_SHORTCUTS"] = "1"
        if args.font_recovery:
            env["CROSSINK_SIMULATOR_SMOKE_FONT_RECOVERY"] = "1"
        env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
        if args.tilt_settings:
            env["CROSSINK_SIMULATOR_SMOKE_TILT_SETTINGS"] = "1"
        if args.receive_folder:
            env["CROSSINK_SIMULATOR_SMOKE_RECEIVE_FOLDER"] = "1"
        if args.receive_folder_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.receive_folder_captures).resolve())
        env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = simulator_book_path
        env["CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS"] = str(args.page_turns)
        if args.context_menu_sync_return:
            env["CROSSINK_SIMULATOR_SMOKE_CONTEXT_MENU_SYNC_RETURN"] = args.context_menu_sync_return
            env["CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_RETURN"] = "1"
        if args.file_browser_sync_return:
            env["CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_RETURN"] = "1"
        if args.frontlight_sync:
            env["CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_SYNC"] = "1"
        if args.frontlight_layout:
            env["CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_LAYOUT"] = "1"
        if args.frontlight_captures:
            capture_dir = Path(args.frontlight_captures).resolve()
            capture_dir.mkdir(parents=True, exist_ok=True)
            env["CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_CAPTURES"] = str(capture_dir)
        if args.opds_catalog:
            env["CROSSINK_SIMULATOR_SMOKE_OPDS_CATALOG"] = "1"
        if args.support_export:
            env["CROSSINK_SIMULATOR_SMOKE_SUPPORT_EXPORT"] = "1"
        if args.bookmarks:
            env["CROSSINK_SIMULATOR_SMOKE_BOOKMARKS"] = "1"
        if args.about:
            env["CROSSINK_SIMULATOR_SMOKE_ABOUT"] = "1"
        if args.release_notes:
            env["CROSSINK_SIMULATOR_SMOKE_RELEASE_NOTES"] = "1"
        if args.about_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.about_captures).resolve())
        if args.sync_server_captures:
            env["CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES"] = str(Path(args.sync_server_captures).resolve())
            env["CROSSINK_SIMULATOR_SMOKE_SYNC_SERVER_CAPTURES"] = "1"
        if args.menu_orientation:
            env["CROSSINK_SIMULATOR_SMOKE_MENU_ORIENTATION"] = "1"
        if args.home_swipe:
            env["CROSSINK_SIMULATOR_SMOKE_HOME_SWIPE"] = "1"
        if args.home_themes:
            env["CROSSINK_SIMULATOR_SMOKE_HOME_THEMES"] = "1"
        if args.theme:
            env["CROSSINK_SIMULATOR_SMOKE_THEME"] = str(THEMES[args.theme])
        if args.headless:
            env.setdefault("SDL_VIDEODRIVER", "dummy")

        print(f"Running simulator smoke test with isolated fs_: {temp_root / 'fs_'}", flush=True)
        proc = subprocess.run(
            [str(program)],
            cwd=temp_root,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout if args.timeout is not None else (300 if args.asset_downloads or args.frontlight_layout else 180 if args.about or args.support_export or args.filename_font_family else 120),
        )

        if factory_flash and factory_flash.read_bytes() != b"\xa5" * 0x14000:
            raise AssertionError("Factory partition was modified")

    print(proc.stdout, end="")

    if proc.returncode != 0:
        print(f"Simulator smoke test failed with exit code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    if args.language_file and f"Mapped {code} " not in proc.stdout:
        print(f"Simulator did not boot with cached language {code}", file=sys.stderr)
        return 2

    for pattern in CRASH_PATTERNS:
        if pattern in proc.stdout:
            print(f"Simulator smoke test output contained crash pattern: {pattern}", file=sys.stderr)
            return 2

    if "Simulator smoke test passed" not in proc.stdout:
        print("Simulator smoke test did not print its success marker", file=sys.stderr)
        return 2

    if args.bookmarks and "Simulator smoke test passed: bookmarks" not in proc.stdout:
        print("Simulator did not complete bookmark checks", file=sys.stderr)
        return 2

    if args.menu_orientation and "Simulator smoke test passed: menu orientation" not in proc.stdout:
        print("Simulator did not complete the menu orientation checks", file=sys.stderr)
        return 2

    if args.release_notes and "Simulator smoke test passed: release notes" not in proc.stdout:
        print("Simulator did not complete the release notes checks", file=sys.stderr)
        return 2

    if args.clipping_selection and "Simulator smoke test passed: touch clipping actions" not in proc.stdout:
        print("Simulator did not complete the touch clipping checks", file=sys.stderr)
        return 2

    asset_marker = ("Simulator smoke test passed: empty catalog" if args.asset_empty_catalog else
                    "Simulator smoke test passed: language save failure" if args.asset_save_failure else
                    "Simulator smoke test passed: asset downloads")
    if args.asset_downloads and asset_marker not in proc.stdout:
        print("Simulator did not complete asset download integration checks", file=sys.stderr)
        return 2

    if args.asset_expect_translation_rejection and "obsolete translation catalog rejected" not in proc.stdout:
        print("Simulator did not reject the obsolete translation contract", file=sys.stderr)
        return 2

    if args.font_recovery and "Simulator smoke test passed: font recovery" not in proc.stdout:
        print("Simulator did not complete the font recovery checks", file=sys.stderr)
        return 2

    if args.c3_font_sizes and "Simulator smoke test passed: bitmap font sizes" not in proc.stdout:
        print("Simulator did not complete the bitmap font-size checks", file=sys.stderr)
        return 2

    if args.reader_font_layout and "Simulator smoke test passed: reader font layout" not in proc.stdout:
        print("Simulator did not complete the Reader Font layout checks", file=sys.stderr)
        return 2

    if args.font_family and not args.c3_font_sizes and not args.reader_font_layout:
        tab_change = proc.stdout.find("Reader Menu tab changed after TTF Native selection")
        reader_return = proc.stdout.find("Reader restored after TTF Native selection", tab_change)
        if tab_change < 0 or reader_return < 0 or "Loading file:" not in proc.stdout[tab_change:reader_return]:
            print("Reader did not reload its page after changing TTF options and switching tabs", file=sys.stderr)
            return 2

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--factory-flash", action="store_true",
                        help="Use the reported 80 KiB factory partition; verify it remains unchanged (S3 profiles)")
    parser.add_argument("--book", default=str(DEFAULT_BOOK), help="EPUB fixture to copy into the isolated simulator fs_")
    parser.add_argument("--env", choices=("simulator", "simulator-X3", "x4-classic-simulator", "sticky-simulator", "x4-pro-simulator"), default="simulator",
                        help="PlatformIO simulator environment to build and run")
    parser.add_argument("--status-bar-lifecycle", action="store_true", help="Exercise reader settings return and reflow")
    parser.add_argument("--tilt-settings", action="store_true", help="Check tilt preference visibility, device editing and persistence")
    parser.add_argument("--chapter-percentage", action="store_true", help="Check chapter percentage slot selection, removal, reload and rendered formats")
    parser.add_argument("--centered-status-bar", action="store_true", help="Check Center Status Bar and Separator rows, popups and persistence")
    parser.add_argument("--status-bar-feature", action="store_true", help="Check status bar visibility, independent sizes, disk reload and rendered theme/orientation matrix")
    parser.add_argument("--screen-calibration", action="store_true", help="Check screen calibration button/touch editing, save, cancel, reset and rotations")
    parser.add_argument("--receive-folder", action="store_true", help="Check receive folder selection, navigation and cancellation")
    parser.add_argument("--receive-folder-captures", help="Directory for receive folder picker framebuffer captures (PGM)")
    parser.add_argument("--status-bar-captures", help="Directory for status bar feature framebuffer captures (PGM)")
    parser.add_argument("--language-file", help="SD language YAML installed before boot for localized smoke checks")
    parser.add_argument("--language-installer", help="Native I18nIntegrationTool built from the same source (required with --language-file)")
    parser.add_argument("--asset-downloads", action="store_true", help="Check mocked online catalog/download/explicit activation across minimal-boot reboots (requires test-only .invalid compiled endpoint)")
    parser.add_argument("--asset-snapshot", help="Producer snapshot directory or snapshot.json copied read-only into disposable HTTP fixtures")
    parser.add_argument("--asset-hyphenation-only", action="store_true", help="Run first-phase hyphenation without requiring a matching translation contract")
    parser.add_argument("--asset-expect-translation-rejection", action="store_true", help="Require supplied obsolete translation catalog to be rejected after hyphenation succeeds")
    parser.add_argument("--asset-empty-catalog", action="store_true", help="Verify a valid empty catalog exits without a transfer or network-error retry")
    parser.add_argument("--asset-save-failure", action="store_true", help="Force settings publication failure after language preparation and verify explicit recovery restart")
    parser.add_argument("--asset-captures", help="Directory for asset-download framebuffer captures (PGM)")
    parser.add_argument("--hyphenation", action="store_true", help="Check SD pack management, pinned flash banks, restart activation and deletion")
    parser.add_argument("--hyphenation-captures", help="Directory for hyphenation manager framebuffer captures (PGM)")
    parser.add_argument("--clipping-selection", action="store_true", help="Check floating text actions, selection handles, dismissal and saving")
    parser.add_argument("--dictionary", action="store_true", help="Check French contractions and exact dictionary matches")
    parser.add_argument("--gesture-shortcuts", action="store_true", help="Check page gesture and front-button shortcut migrations, input, catalogs and UI")
    parser.add_argument("--c3-font-sizes", "--bitmap-font-sizes", dest="c3_font_sizes", action="store_true", help="Check bitmap font sizes, inline layout, page cache, selection and SD reload")
    parser.add_argument("--font-size-book", action="store_true", help="Also render every page of --book with C3 fonts, checking its opening drop cap")
    parser.add_argument("--font-dir", help="Font fixtures copied into isolated /fonts")
    parser.add_argument("--filename-font-dir", help="TTF family fixtures copied into /.crosspoint/languages/fonts")
    parser.add_argument("--filename-font-family", help="Check filename font picker, mixed glyphs and storage lifecycle (S3)")
    parser.add_argument("--filename-font-captures", help="Directory for filename font framebuffer captures (PGM)")
    parser.add_argument("--font-recovery", action="store_true", help="Check reader selection recovery after font catalog memory pressure")
    parser.add_argument("--font-family", help="Exercise custom-font size and dictionary lifecycle")
    parser.add_argument("--reader-font-layout", action="store_true", help="Check Reader Font row bounds, scrolling and TTF touch navigation across orientations and UI sizes (X4 Pro; requires --font-dir and --font-family)")
    parser.add_argument("--timeout", type=int, help="Seconds before the simulator run is treated as hung (default: 120; 180 for about, support export or filename fonts; 300 for frontlight layout or asset downloads)")
    parser.add_argument("--page-turns", type=int, default=2, help="Number of EPUB page-forward taps to run")
    parser.add_argument("--theme", choices=sorted(THEMES), help="UI theme to use during the smoke test")
    parser.add_argument("--context-menu-sync-return", choices=["file-browser", "library"], help="Check context-menu sync return across network reboots")
    parser.add_argument("--file-browser-sync-return", action="store_true", help="Check browser sync return across network reboots")
    parser.add_argument("--frontlight-sync", action="store_true", help="Check frontlight sync outside the reader with stats enabled and disabled (X4 Pro)")
    parser.add_argument("--frontlight-layout", action="store_true", help="Check frontlight drawer bounds, handle swipes/taps, reader return and explicit Home across scales, orientations and themes (X4 Pro)")
    parser.add_argument("--frontlight-captures", help="Directory for frontlight layout framebuffer captures (PGM)")
    parser.add_argument("--opds-catalog", action="store_true", help="Check OPDS catalog release and download return flows")
    parser.add_argument("--support-export", action="store_true", help="Check support export privacy, scopes, cancellation and failure preservation")
    parser.add_argument("--bookmarks", action="store_true", help="Check long bookmark lists from Home and the reader, scrolling, deletion and jumps; captures via --about-captures")
    parser.add_argument("--about", action="store_true", help="Check About navigation, paging and read-only snapshots across scales/orientations")
    parser.add_argument("--release-notes", action="store_true", help="Check the one-time What's New screen (Download, Update, touch Later vs swipe, landscape hints) and the Settings Release Notes entry; captures via --about-captures")
    parser.add_argument("--about-captures", help="Directory for About framebuffer captures (PGM)")
    parser.add_argument("--sync-server-captures", help="Directory for Sync Server settings captures (PGM)")
    parser.add_argument("--menu-orientation", action="store_true", help="Check physical menu labels/actions in all rotations independently of reader preferences; captures via --about-captures")
    parser.add_argument("--home-swipe", action="store_true", help="Check bottom-edge Home swipes, ordinary list swipes, and reader menu gestures on touch devices")
    parser.add_argument("--home-themes", action="store_true", help="Compare drawer theme changes with fresh Home renders (X4 Pro)")
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument("--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video")
    parser.set_defaults(build=True, headless=True)
    args = parser.parse_args()
    if args.reader_font_layout and (args.env != "x4-pro-simulator" or not args.font_dir or not args.font_family):
        parser.error("--reader-font-layout requires --env x4-pro-simulator, --font-dir and --font-family")
    if (args.asset_snapshot or args.asset_hyphenation_only or args.asset_captures or args.asset_expect_translation_rejection or args.asset_empty_catalog or args.asset_save_failure) and not args.asset_downloads:
        parser.error("asset fixture/capture options require --asset-downloads")
    if args.asset_expect_translation_rejection and (args.asset_hyphenation_only or not args.asset_snapshot):
        parser.error("--asset-expect-translation-rejection requires a snapshot and the translation stage")
    if args.asset_empty_catalog and (args.asset_expect_translation_rejection or args.asset_hyphenation_only):
        parser.error("--asset-empty-catalog is a separate scenario")
    if args.asset_save_failure and (args.asset_expect_translation_rejection or args.asset_hyphenation_only or args.asset_empty_catalog):
        parser.error("--asset-save-failure requires a successful translation stage")
    if args.asset_downloads and args.hyphenation:
        parser.error("--asset-downloads and --hyphenation are separate smoke scenarios")
    if args.c3_font_sizes and args.env not in ("simulator", "simulator-X3") and not (args.font_dir and args.font_family):
        parser.error("--bitmap-font-sizes on S3 requires --font-dir and --font-family with cpfont fixtures")
    if args.font_size_book and not args.c3_font_sizes:
        parser.error("--font-size-book requires --c3-font-sizes")
    if args.language_file and not args.language_installer:
        parser.error("--language-file requires --language-installer")
    return args


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
