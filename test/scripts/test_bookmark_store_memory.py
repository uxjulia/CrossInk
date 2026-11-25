#!/usr/bin/env python3
"""Compile and run the production bookmark store with forced allocation failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="crossink-bookmark-store-") as folder:
    folder = Path(folder)
    checksum = folder / "checksums.o"
    subprocess.run(["cc", "-c", str(ROOT / "lib/uzlib/src/uzlib_checksums.c"),
                    "-I" + str(ROOT / "lib/uzlib/src"), "-o", str(checksum)], check=True)
    binary = folder / "test"
    includes = ["test/bookmark_store/stubs", "src", "lib/Memory", "lib/Serialization", "lib/uzlib/src"]
    subprocess.run(["c++", "-std=c++20", "-fno-exceptions", "-fsanitize=address,undefined", "-g",
                    *["-I" + str(ROOT / path) for path in includes],
                    str(ROOT / "test/bookmark_store/BookmarkStoreTest.cpp"),
                    str(ROOT / "src/BookmarkStore.cpp"), str(ROOT / "lib/Serialization/AtomicFile.cpp"), str(checksum), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
