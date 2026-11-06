"""Exercise the production tilt detector with X3 and Sticky gyro calibrations."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "test/tilt_sensitivity"

with tempfile.TemporaryDirectory(prefix="crossink-tilt-sensitivity-") as tmp:
    for sticky in (False, True):
        binary = Path(tmp) / ("sticky" if sticky else "x3")
        subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", f"-DFREEINK_DEVICE_STICKY={int(sticky)}",
                        "-I" + str(TEST / "stubs"), "-I" + str(ROOT / "include"),
                        "-I" + str(ROOT / "lib/hal"), "-I" + str(ROOT / "src/activities/reader"),
                        str(TEST / "TiltSensitivityTest.cpp"), str(ROOT / "lib/hal/HalTiltSensor.cpp"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("Tilt sensitivity: X3 and Sticky detector checks passed with ASan/UBSan")
