"""Check translated date formatting with real caches across fresh boots."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

program, root = sys.argv[1], Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="crossink-dates-") as tmp:
    base = Path(tmp)
    sources = base / "sd/.crossink/languages"
    sources.mkdir(parents=True)
    env = dict(os.environ, CROSSINK_TEST_SD=str(base / "sd"),
               CROSSINK_LANGUAGE_FLASH=str(base / "flash.bin"))
    subprocess.run([program, "EN"], env=env, check=True)
    for source in sorted((root / "lib/I18n/translations").glob("*.yaml")):
        if source.name == "english.yaml":
            continue
        shutil.copy2(source, sources / source.name)
        code = next(line.split('"')[1] for line in source.read_text().splitlines()
                    if line.startswith("_language_code:"))
        subprocess.run([program, "install", "/.crossink/languages/" + source.name], env=env, check=True)
        (sources / source.name).unlink()
        subprocess.run([program, code], env=env, check=True)
