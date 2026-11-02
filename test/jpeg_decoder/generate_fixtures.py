"""Regenerate original stripe fixtures with libjpeg-turbo's cjpeg (no book data)."""
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CJPEG = shutil.which("cjpeg") or "/opt/homebrew/opt/jpeg-turbo/bin/cjpeg"
LUMA_SCAN = "0: 0 0 0 0; 1 2: 0 0 0 0; 0: 1 63 0 0; 1: 1 63 0 0; 2: 1 63 0 0;"
INTERLEAVED_SCAN = "0 1 2: 0 0 0 0; 0: 1 63 0 0; 1: 1 63 0 0; 2: 1 63 0 0;"

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    for name, width, height, sample, scan in [
        ("luma-422", 24, 40, "2x1,1x1,1x1", LUMA_SCAN),
        ("luma-420", 24, 40, "2x2,1x1,1x1", LUMA_SCAN),
        ("luma-440", 24, 40, "1x2,1x1,1x1", LUMA_SCAN),
        ("luma-444", 24, 40, "1x1,1x1,1x1", LUMA_SCAN),
        ("luma-422-partial", 25, 41, "2x1,1x1,1x1", LUMA_SCAN),
        ("luma-422-wide", 1432, 80, "2x1,1x1,1x1", LUMA_SCAN),
        ("interleaved-422", 24, 40, "2x1,1x1,1x1", INTERLEAVED_SCAN),
        ("interleaved-420", 24, 40, "2x2,1x1,1x1", INTERLEAVED_SCAN),
        ("baseline-422", 24, 40, "2x1,1x1,1x1", None),
    ]:
        # Constant 8x8 blocks: every decoded row must retain these column values.
        pixels = bytes(v for y in range(height) for x in range(width)
                       for v in [(32, 128, 224)[(x // 8) % 3]] * 3)
        source = temp / "stripes.ppm"
        source.write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)
        command = [CJPEG, "-quality", "100", "-sample", sample]
        if scan:
            scans = temp / "scans.txt"
            scans.write_text(scan)
            command += ["-scans", str(scans)]
        command += ["-outfile", str(ROOT / "fixtures" / f"{name}.jpg"), str(source)]
        subprocess.run(command, check=True)
