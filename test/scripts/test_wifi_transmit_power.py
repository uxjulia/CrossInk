"""Exercise the production radio-start hook, including retained power limits."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "test/wifi_transmit_power"
SCENARIOS = ("default", "before-scan", "restore", "board-ceiling", "bad-setting",
             "start-failure", "read-failure", "bad-driver-power", "set-failure")
PROFILE_SCENARIOS = ("profile-default", "profile-restore", "profile-lower-radio", "profile-read-failure")

with tempfile.TemporaryDirectory(prefix="crossink-wifi-power-") as tmp:
    binary = Path(tmp) / "test"
    for board_limit, scenarios in ((0, SCENARIOS), (60, PROFILE_SCENARIOS)):
        subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", f"-DTEST_WIFI_BOARD_LIMIT={board_limit}",
                        "-I" + str(TEST / "stubs"), "-I" + str(ROOT / "include"),
                        str(TEST / "WifiTransmitPowerTest.cpp"),
                        str(ROOT / "lib/hal/HalWifi.cpp"), "-o", str(binary)], check=True)
        for scenario in scenarios:
            subprocess.run([str(binary), scenario], check=True)
print(f"Wi-Fi transmit power: {len(SCENARIOS) + len(PROFILE_SCENARIOS)} scenarios passed")
