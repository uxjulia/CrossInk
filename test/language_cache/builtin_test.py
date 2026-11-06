#!/usr/bin/env python3
"""Compile actual I18n with default, subset and all tables; exercise SD precedence."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("generator", root / "scripts/gen_i18n.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
identities = generator.known_languages()
codes = [item[0] for item in identities]
sources = root / "lib/I18n/translations"
assert generator.parse_builtin_langs("es,FR", codes) == {"EN", "ES", "FR"}
assert generator.parse_builtin_langs("all", codes) is None
try:
    generator.parse_builtin_langs("en,typo", codes)
except ValueError:
    pass
else:
    raise AssertionError("Unknown language must fail")

with tempfile.TemporaryDirectory(prefix="crossink-builtins-") as temporary:
    base = Path(temporary)
    for setting, expected in [(None, ["EN"]), ("es,fr", ["EN", "ES", "FR"]), ("all", codes)]:
        build = base / (setting or "default")
        build.mkdir()
        command = [sys.executable, str(root / "scripts/gen_i18n.py"), str(sources), str(build),
                   "--strip-unused", "--src-dirs", str(root / "src"), str(root / "lib")]
        if setting is not None:
            command += ["--builtin-langs", setting]
        subprocess.run(command, check=True, capture_output=True)
        binary = build / "i18n"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-DSIMULATOR", "-Wall", "-Wextra", "-Werror",
                        *["-I" + str(p) for p in [root / "test/language_cache/stubs", build, root / "lib/I18n",
                          root / "lib/LanguageCache", root / "lib/AssetCacheStorage", root / "lib/HalFlashPartition", root / "lib/Memory"]],
                        str(root / "test/language_cache/I18nIntegrationTool.cpp"), str(root / "lib/I18n/I18n.cpp"),
                        str(build / "I18nStrings.cpp"), str(root / "lib/LanguageCache/LanguageCache.cpp"),
                        str(root / "lib/AssetCacheStorage/AssetCacheStorage.cpp"), str(root / "lib/HalFlashPartition/HalFlashPartition.cpp"), "-o", str(binary)],
                       check=True, capture_output=True)
        sd = build / "sd/.crossink/languages"
        sd.mkdir(parents=True)
        flash = build / "flash.bin"
        env = dict(os.environ, CROSSINK_TEST_SD=str(build / "sd"), CROSSINK_LANGUAGE_FLASH=str(flash))

        def run(*args):
            result = subprocess.run([str(binary), *map(str, args)], env=env, text=True, capture_output=True)
            assert result.returncode == 0, (setting, args, result.stdout, result.stderr)
            return result.stdout.strip()

        def lookup(code, key, generation=0):
            return run("lookup", code, generation, key)

        # The Python generator and C++ installer must enforce the same printf contract.
        formats = ["", "plain", "%%", "%s", "%u", "%i", "%d", "%x", "%lu", "%llu",
                   "%zu", "%hhd", "%Lf", "%*.*s", "%1024.128f", "%1025u", "%.129s",
                   "%n", "%1$s", "%", "%q", "%s" * 32, "%s" * 33]
        if setting is None:
            for reference in formats:
                for translated in formats:
                    a, b = generator.format_signature(reference), generator.format_signature(translated)
                    expected_format = a is not None and b is not None and a == b
                    assert run("format", reference, translated) == str(int(expected_format)), (reference, translated)

        choices = run("scan").splitlines()
        assert choices[0].startswith("EN|")
        assert {line.split("|")[0] for line in choices} == set(expected), (setting, choices, expected)
        assert not flash.exists(), "Reading a builtin must not provision flash"
        for code in expected:
            assert run("boot", code).split("|")[0] == code
        french = generator.parse_yaml_file(str(sources / "french.yaml"))
        fallback = french["STR_SETTINGS_TITLE"] if "FR" in expected else "Settings"
        assert lookup("FR", "STR_SETTINGS_TITLE") == fallback
        if "AR" in expected:
            assert run("boot", "AR").endswith("|1")
            assert run("boot", "HE").endswith("|1")
        # An SD source with the SAME identity must override embedded translations.
        sample = sd / "french.yaml"
        sample.write_text('_language_code: "FR"\n_language_name: "SD French"\n'
                          'STR_SETTINGS_TITLE: "SD TEST FRENCH"\n')
        choices = run("scan").splitlines()
        assert sum(line.startswith("FR|") for line in choices) == 1
        assert any(line.startswith("FR|SD French|0|/.crossink/languages/") for line in choices)
        assert run("install", "FR", 0, "/.crossink/languages/french.yaml") == "FR|1"
        assert lookup("FR", "STR_SETTINGS_TITLE", 1) == "SD TEST FRENCH"
        assert lookup("FR", "STR_MENU", 1) == "Menu", "Missing cache key uses English"
        sample.write_text(sample.read_text().replace("SD TEST FRENCH", "SD REVISED"))
        assert lookup("FR", "STR_SETTINGS_TITLE", 1) == "SD TEST FRENCH"
        assert run("install", "FR", 1, "/.crossink/languages/french.yaml") == "FR|2"
        assert lookup("FR", "STR_SETTINGS_TITLE", 1) == "SD TEST FRENCH"
        sample.unlink()
        assert lookup("FR", "STR_SETTINGS_TITLE", 2) == "SD REVISED"
        choices = run("scan", "FR", 2).splitlines()
        assert sum(line.startswith("FR|") for line in choices) == 1
        assert any(line.startswith("FR|SD French|0|") for line in choices)
        assert lookup("FR", "STR_SETTINGS_TITLE", 999) == fallback
        flash.write_bytes(b"\xff" * 0x360000)
        assert lookup("FR", "STR_SETTINGS_TITLE", 2) == fallback
        flash.unlink()
        # Existing 28 release sources plus all builtins fit the bounded catalog.
        for path in sources.glob("*.yaml"):
            if path.name != "english.yaml":
                shutil.copy2(path, sd / path.name)
        assert len(run("scan").splitlines()) == 29

    # Invalid options must fail the CLI, not quietly produce English-only output.
    result = subprocess.run(command[:-2] + ["--builtin-langs", "NOT-A-LANGUAGE"], capture_output=True)
    assert result.returncode != 0
    selected_sources = base / "selected-sources"
    selected_sources.mkdir()
    shutil.copy2(sources / "english.yaml", selected_sources)
    for code in ["FR", "ES"]:
        filename = "french.yaml" if code == "FR" else "spanish.yaml"
        shutil.copy2(sources / filename, selected_sources)
    (selected_sources / "unselected.yaml").write_text('not a valid yaml file')
    (selected_sources / "omitted-hebrew.yaml").write_bytes(b'_language_code: "HE"\nSTR_SETTINGS_TITLE: "\xff"\n')
    generator.load_translations(str(selected_sources), builtin={"EN", "FR", "ES"})
    shutil.copy2(selected_sources / "french.yaml", selected_sources / "french-copy.yaml")
    try:
        generator.load_translations(str(selected_sources), builtin={"EN", "FR"})
    except ValueError as error:
        assert "Duplicate" in str(error)
    else:
        raise AssertionError("Duplicate selected identity must fail")
    (selected_sources / "french-copy.yaml").unlink()
    (selected_sources / "french.yaml").unlink()
    try:
        generator.load_translations(str(selected_sources), builtin={"EN", "FR"})
    except ValueError as error:
        assert "Missing" in str(error)
    else:
        raise AssertionError("Missing selected identity must fail")
print("Builtin matrix passed: default EN, explicit subset, all 29, invalid config, SD precedence/reboot/fallback")
