#!/usr/bin/env python3
"""Check disabled-touch reader recovery through the simulator's real input loop."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_recovery(program: Path, disabled: bool, release_up_first: bool, text_book: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="crossink-touch-recovery-") as directory:
        work = Path(directory)
        state = work / "fs_/.crosspoint"
        state.mkdir(parents=True)
        book_path = "/book.txt" if text_book else "/book.epub"
        if text_book:
            (work / "fs_/book.txt").write_text("Touchscreen recovery test.\n" * 200)
        else:
            shutil.copy2(ROOT / "test/epubs/test_reader_rendering_matrix.epub", work / "fs_/book.epub")
        settings_file = state / "crossink-settings.json"
        settings_file.write_text(json.dumps({
            "disableReaderTouchscreen": int(disabled),
            "sideButtonChordAction": 35,  # Rotate Page 180 degrees
            "orientation": 0,
        }))
        (state / "state.json").write_text(json.dumps({
            "openEpubPath": book_path, "lastSleepFromReader": True, "showBootScreen": False,
        }))
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("CROSSPOINT_SIM_", "CROSSINK_SIMULATOR_SMOKE"))}
        up_hold, down_hold = (120, 240) if release_up_first else (240, 120)
        dismiss = "4000:SWIPE:0.5,0.98,0.5,0.6,250" if text_book else "4000:TAP:0.5,0.1,80"
        env.update(
            SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_WAKE_REASON="power",
            CROSSPOINT_SIM_INPUT_SCRIPT=(
                f"2500:UP:{up_hold};2500:DOWN:{down_hold};"
                f"{dismiss};5500:QUIT"
            ),
        )
        result = subprocess.run([str(program)], cwd=work, env=env, capture_output=True,
                                text=True, timeout=20)
        output = result.stdout + result.stderr
        if result.returncode:
            raise AssertionError(f"Simulator failed\n{output}")
        menu_name = "Settings" if text_book else "EpubReaderDrawer"
        opened = f"Entering activity: {menu_name}" in output
        if opened != disabled:
            raise AssertionError(f"disabled={disabled}: unexpected recovery menu state\n{output}")
        if disabled and f"Exiting activity: {menu_name}" not in output:
            raise AssertionError(f"Recovery menu could not be dismissed by touch\n{output}")
        saved = json.loads(settings_file.read_text())
        if bool(saved.get("disableReaderTouchscreen", 0)) != disabled:
            raise AssertionError("Recovery changed the persistent touchscreen setting")
        orientation = saved.get("orientation", 0)
        if not text_book and not disabled:
            # EPUB rotation is a per-book preference. The snapshot follows a
            # five-byte header; orientation follows four one-byte font fields.
            snapshots = list(state.glob("epub_*/reader_settings.bin"))
            if len(snapshots) != 1:
                raise AssertionError("Missing per-book rotation settings")
            orientation = snapshots[0].read_bytes()[9]
        expected_orientation = 0 if disabled else 2
        if orientation != expected_orientation:
            raise AssertionError(f"Expected orientation {expected_orientation}, got {orientation}")
        print(f"PASS: text_book={text_book}, disabled={disabled}, release_up_first={release_up_first}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", choices=("x4-pro-simulator", "sticky-simulator"), default="x4-pro-simulator")
    args = parser.parse_args()
    program = ROOT / ".pio/build" / args.env / "program"
    for text_book in (False, True):
        for disabled in (True, False):
            for release_up_first in (True, False):
                check_recovery(program, disabled, release_up_first, text_book)


if __name__ == "__main__":
    main()
