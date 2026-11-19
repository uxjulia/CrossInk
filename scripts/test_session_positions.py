#!/usr/bin/env python3
"""Read forward then backward in the X4 simulator and check its pending session records."""
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import zlib

from test_chapter_shortcuts import epub, xtc

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / ".pio/build/simulator/program"
SESSION_V2 = struct.Struct("<B32s32sQQIhhhI")


def read_sessions(work, suffix, button):
    state = work / "fs_" / ".crosspoint"
    (state / "state.json").write_text(json.dumps({
        "openEpubPath": f"/book.{suffix}", "lastSleepFromReader": True, "releaseNotesSeen": 1,
    }))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_SD=str(work / "fs_"),
               CROSSPOINT_SIM_WAKE_REASON="power",
               CROSSPOINT_SIM_INPUT_SCRIPT=f"7000:{button};14500:BACK;16000:QUIT")
    result = subprocess.run([str(PROGRAM)], cwd=work, env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    journals = list((work / "fs_").glob("*/reading_sessions_*"))
    assert len(journals) == 1, result.stdout[-5000:]
    sessions = {}
    for path in journals[0].glob("*.bin"):
        raw = path.read_bytes()
        version, session_id, _, _, _, _, _, start, end, crc = SESSION_V2.unpack(raw)
        assert version == 2 and crc == zlib.crc32(raw[:-4]), path
        assert session_id.decode("ascii") == path.stem, path
        sessions[path.stem] = (start, end)
    return sessions


def check(suffix, fixture):
    with tempfile.TemporaryDirectory(prefix="crossink-session-") as tmp:
        work = Path(tmp)
        state = work / "fs_" / ".crosspoint"
        state.mkdir(parents=True)
        (state / "crossink-settings.json").write_text(json.dumps({"trackReadingStats": 1}))
        fixture(work / "fs_" / f"book.{suffix}")
        first = read_sessions(work, suffix, "RIGHT")
        assert len(first) == 1, first
        start, end = next(iter(first.values()))
        assert 0 <= start < end <= 1000, first
        second = read_sessions(work, suffix, "LEFT")
        assert first.items() <= second.items(), second
        assert len(second) == 2, second
        reverse = second[next(iter(second.keys() - first.keys()))]
        assert reverse == (end, start), (first, second)
        print(f"PASS {suffix}: {start} -> {end} -> {start} permille", flush=True)


if __name__ == "__main__":
    check("epub", epub)
    check("xtc", xtc)
