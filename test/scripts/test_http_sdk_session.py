#!/usr/bin/env python3
"""Exercise the pinned SDK client with the app's session framing policy."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
sdk = root / "freeink-sdk/libs/network/SecureNet"
with tempfile.TemporaryDirectory(prefix="crossink-sdk-session-") as temp:
    binary = Path(temp) / "sdk-session"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                    "-I" + str(sdk / "test/host/stubs"), "-I" + str(sdk / "include"),
                    "-I" + str(root / "src"),
                    str(root / "test/http_downloader/sdk_session/SdkSessionTest.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
