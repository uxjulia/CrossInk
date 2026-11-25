#!/usr/bin/env python3
"""Check real firmware settings migration and cached-language boot in the simulator."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(program, installer, output):
    output.mkdir(parents=True, exist_ok=True)
    identities = json.loads((ROOT / "lib/I18n/languages.json").read_text())
    french_index = [item[0] for item in identities].index("FR")
    cases = ["custom", "missing-cache", "numeric", "legacy-binary", "unfinished-save", "corrupt-cache"]
    with tempfile.TemporaryDirectory(prefix="crossink-language-boots-") as temporary:
        for case in cases:
            work = Path(temporary) / case
            state = work / "fs_/.crosspoint"
            state.mkdir(parents=True)
            languages = work / "fs_/.crossink/languages"
            languages.mkdir(parents=True)
            cache = work / "flash.bin"
            env = dict(os.environ, SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_INPUT_SCRIPT="3000:QUIT",
                       CROSSINK_LANGUAGE_FLASH=str(cache), CROSSINK_TEST_SD=str(work / "fs_"))
            preference = {"language": "ZZ-CUSTOM", "languageCacheGeneration": 1}
            expected_code = "ZZ-CUSTOM"
            if case not in ("missing-cache", "numeric", "legacy-binary", "unfinished-save"):
                source = languages / "custom.yaml"
                source.write_text('_language_code: "ZZ-CUSTOM"\n_language_name: "Community UI"\n'
                                  '_direction: "rtl"\n_keyboard: "HE"\nSTR_SETTINGS_TITLE: "Custom settings"\n')
                prepared = subprocess.run([str(installer), "install", "EN", "0", "/.crossink/languages/custom.yaml"],
                                          env=env, cwd=work, text=True, capture_output=True, check=True)
                assert prepared.stdout.strip() == "ZZ-CUSTOM|1", prepared.stdout
                source.unlink()  # firmware must not require or rediscover the source
            if case == "numeric":
                preference = {"language": french_index}
                expected_code = "FR"
            elif case == "legacy-binary":
                preference = None
                (state / "language.bin").write_bytes(bytes([1, 5]))  # frozen v1 index 5 = PT (Brazil)
                expected_code = "PT"
            elif case == "unfinished-save":
                preference = {"language": "DE"}
                (state / "crossink-settings.json.bak").write_text(json.dumps({"language": "FR"}))
                expected_code = "FR"
            elif case == "corrupt-cache":
                with cache.open("r+b") as stream:
                    stream.seek(0x340000 + 256 + 18)
                    old = stream.read(1)
                    stream.seek(-1, 1)
                    stream.write(bytes([old[0] ^ 1]))
            if preference is not None:
                (state / "crossink-settings.json").write_text(json.dumps(preference))
            process = subprocess.run([str(program)], env=env, cwd=work, text=True, capture_output=True, timeout=25)
            log = process.stdout + process.stderr
            (output / f"{case}.log").write_text(log)
            assert process.returncode == 0, (case, process.returncode, log[-3000:])
            assert "Segmentation fault" not in log and "Assertion failed" not in log, case
            saved = json.loads((state / "crossink-settings.json").read_text())
            assert saved["language"] == expected_code, (case, saved["language"])
            if case == "custom":
                assert "Mapped ZZ-CUSTOM" in log and saved["languageCacheGeneration"] == 1, log
            if case in ("missing-cache", "corrupt-cache"):
                assert "using English" in log, log
            if case == "unfinished-save":
                assert not (state / "crossink-settings.json.bak").exists()
            print(f"PASS {case}: preferred language {expected_code}", flush=True)


def run_ui(program, output):
    """Drive the real button picker; terminate after observing the restarted Home."""
    output.mkdir(parents=True, exist_ok=True)
    actions = ["UP", "ENTER", "ENTER", "ENTER", "ENTER", "DOWN", "ENTER", "DOWN", "DOWN", "DOWN", "ENTER"]
    navigation = [f"{1000 + i * 500}:{key}" for i, key in enumerate(actions)]
    with tempfile.TemporaryDirectory(prefix="crossink-language-picker-") as temporary:
        work = Path(temporary)
        state = work / "fs_/.crosspoint"
        state.mkdir(parents=True)
        languages = work / "fs_/.crossink/languages"
        languages.mkdir(parents=True)
        settings = state / "crossink-settings.json"
        settings.write_text(json.dumps({"language": "EN", "uiTheme": 0}))
        source = languages / "custom.yaml"
        for case in ["install", "reapply", "missing-source", "english", "invalid", "save-failure", "catalog-limit"]:
            source.write_text('_language_code: "ZZ"\n_language_name: "Community test"\n'
                              f'STR_SETTINGS_TITLE: "Custom settings {case}"\n'
                              'STR_LANGUAGE_APPLY_HINT: "Apply"\n')
            if case == "catalog-limit":
                for i in range(16):
                    (languages / (f"long-{i}-" + "x" * 90 + ".yaml")).write_text(
                        f'_language_code: "LONG-{i}"\n_language_name: "' + "x" * 90 + '"\n')
            before = json.loads(settings.read_text())
            expected_code = "EN" if case in ("english", "invalid", "save-failure", "catalog-limit") else "ZZ"
            expected_generation = 0 if case == "english" else (before.get("languageCacheGeneration", 0) + 1
                                  if case in ("install", "reapply") else before.get("languageCacheGeneration", 0))
            if case == "missing-source":
                source.unlink()
            if case == "invalid":
                with source.open("a") as stream:
                    stream.write('STR_SETTINGS_TITLE: "duplicate"\n')
            events = list(navigation)
            if case in ("install", "invalid", "save-failure"):
                events.append("6500:DOWN")
            if case == "english":
                events.append("6500:UP")
            events += ["7000:ENTER", "14000:QUIT"]
            env = dict(os.environ, SDL_VIDEODRIVER="dummy", CROSSINK_LANGUAGE_FLASH=str(work / "flash.bin"),
                       CROSSPOINT_SIM_INPUT_SCRIPT=";".join(events),
                       CROSSPOINT_SIM_SCREENSHOTS=f"6200:{output / (case + '-picker.bmp')}")
            log_path = output / f"ui-{case}.log"
            blocked_save = False
            with log_path.open("w") as log:
                process = subprocess.Popen([str(program)], cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
                start = time.monotonic()
                try:
                    while process.poll() is None and time.monotonic() - start < 14:
                        elapsed = time.monotonic() - start
                        if case == "save-failure" and elapsed > 6.3 and not blocked_save:
                            Path(str(settings) + ".tmp").mkdir()
                            blocked_save = True
                        contents = log_path.read_text()
                        if case in ("invalid", "save-failure"):
                            if elapsed > 9:
                                break
                        elif contents.count("Entering activity: Home") >= 2:
                            break
                        time.sleep(0.05)
                finally:
                    process.terminate()
                    process.wait(timeout=5)
            contents = log_path.read_text()
            saved = json.loads(settings.read_text())
            assert saved["language"] == expected_code, (case, saved, contents[-3000:])
            assert saved.get("languageCacheGeneration", 0) == expected_generation, (case, saved)
            assert "Assertion failed" not in contents and "Segmentation fault" not in contents, contents
            if case in ("invalid", "save-failure"):
                assert contents.count("Entering activity: Home") == 1, contents
                assert "duplicate key" in contents if case == "invalid" else "crossink-settings.json.tmp" in contents
            else:
                assert contents.count("Entering activity: Home") >= 2, contents
                if expected_code == "ZZ":
                    assert "Mapped ZZ" in contents, contents
            if case == "catalog-limit":
                assert "Cannot show language catalog" in contents and "exceeds limits" in contents
            if blocked_save:
                Path(str(settings) + ".tmp").rmdir()
            print(f"PASS UI {case}: {expected_code} generation {expected_generation}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, default=ROOT / ".pio/build/simulator/program")
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/language-boot-tests")
    parser.add_argument("--ui", action="store_true", help="Also drive apply/reapply/English/failure through the real picker")
    args = parser.parse_args()
    run(args.program.resolve(), args.installer.resolve(), args.output.resolve())

    if args.ui:
        run_ui(args.program.resolve(), args.output.resolve())
