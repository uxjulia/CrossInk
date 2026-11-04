#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

root = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("generator", root / "scripts/gen_i18n.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
with tempfile.TemporaryDirectory(prefix="crossink-language-generator-") as temporary:
    base = Path(temporary)
    (base / "english.yaml").write_bytes((root / "lib/I18n/translations/english.yaml").read_bytes())
    (base / "broken-community.yaml").write_text("this is not valid YAML")
    languages, names, keys, _, _ = generator.load_translations(str(base), builtin={"EN"})
    assert languages == [item[0] for item in generator.known_languages()]
    assert names[0] == "English" and "STR_SETTINGS_TITLE" in keys
    for key in ("STR_HOLD_FOR_KEYBOARD", "STR_LIBRARY_SCAN_COUNT", "STR_LIBRARY_FILES_COUNT",
                "STR_STATS_UPLOAD_COUNTS", "STR_FOLDER_SYNC_COUNTS"):
        assert key in generator.FORMAT_KEYS
    # Device compatibility is tied to the original English source and complete
    # ordered source keyset, independent of strip-unused and built-in choices.
    generated = base / "generated"
    generated.mkdir()
    for flags in ([], ["--strip-unused"]):
        subprocess.run([sys.executable, str(root / "scripts/gen_i18n.py"), str(base), str(generated),
                        "--src-dirs", str(root / "src"), str(root / "lib"), *flags],
                       check=True, capture_output=True)
        cpp = (generated / "I18nStrings.cpp").read_text()
        english = (base / "english.yaml").read_bytes()
        source_keys = [key for key in generator.parse_yaml_file(str(base / "english.yaml"))
                       if key.startswith("STR_")]
        for name, data in (("SOURCE_SHA256", english),
                           ("KEYSET_SHA256", ("\n".join(source_keys) + "\n").encode())):
            assert f'const char {name}[] = "{hashlib.sha256(data).hexdigest()}";' in cpp
    compatibility = generator.known_languages()
    assert compatibility[0] == ["EN", "English"]
    assert dict(compatibility)["PT"] == "Português (Brasil)"
    assert dict(compatibility)["PT2"] == "Português (Portugal)"
    assert dict(compatibility)["SI"] == "Slovenščina"
    for name in ["first.zip", "second.zip"]:
        subprocess.run([sys.executable, str(root / "scripts/package_languages.py"), "--version", "test",
                        "--output", str(base / name)], check=True, capture_output=True)
    assert (base / "first.zip").read_bytes() == (base / "second.zip").read_bytes()
    with zipfile.ZipFile(base / "first.zip") as archive:
        names = archive.namelist()
        assert not any(n.startswith(".crosspoint/") for n in names)
        assert "english-template.yaml" in names
        assert ".crossink/languages/english.yaml" not in names
        assert len([n for n in names if n.startswith(".crossink/languages/")]) == 27
        assert json.loads(archive.read("manifest.json"))["firmware"] == "test"
print("Generator and packaging passed: English-only dependency, legacy identities, and reproducible release archive")
