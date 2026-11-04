#!/usr/bin/env python3
"""Check translation completeness and generated firmware lookups, including large blobs."""

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_i18n


class I18nTest(unittest.TestCase):
    def test_translation_completeness_and_formats(self):
        directory = ROOT / "lib/I18n/translations"
        english = gen_i18n.parse_yaml_file(str(directory / "english.yaml"))
        # Space is deliberately excluded: "99% of" is prose, not a printf conversion.
        specifier = re.compile(
            r"%(?:\d+\$)?[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
            r"(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp%]"
        )
        path = re.compile(
            r"/\.crossink/(?:languages(?:/fonts)?|hyphenation)|"
            r"/crossink-support\.json|\.bak|crash_report\.txt|firmware\.bin|/Read\b|\.bin\b"
        )
        for file in sorted(directory.glob("*.yaml")):
            data = gen_i18n.parse_yaml_file(str(file))
            for key, source in english.items():
                if not key.startswith("STR_"):
                    continue
                with self.subTest(language=file.stem, key=key):
                    self.assertIn(key, data)
                    self.assertTrue(data[key].strip())
                    if specifier.findall(source):
                        self.assertEqual(specifier.findall(source), specifier.findall(data[key]))
                    self.assertEqual(source.count("\n"), data[key].count("\n"))
                    for token in path.findall(source):
                        self.assertIn(token, data[key])

    def check_runtime(self, languages, names, keys, translations, builtin=None):
        with tempfile.TemporaryDirectory(prefix="crossink-i18n-") as tmp:
            output = Path(tmp)
            gen_i18n.generate_keys_header(languages, names, keys, str(output / "I18nKeys.h"), builtin=builtin)
            gen_i18n.generate_strings_header(languages, names, str(output / "I18nStrings.h"), builtin=builtin)
            gen_i18n.generate_strings_cpp(languages, names, keys, translations, str(output / "I18nStrings.cpp"), builtin=builtin)
            (output / "check.cpp").write_text(
                '#include "I18n.h"\n#include <cstdio>\n#include <cstring>\n'
                'int main(int argc, char** argv) {\n'
                '  auto& i18n = I18n::getInstance();\n'
                '  if (argc != 2) return 1;\n'
                '  i18n.begin(argv[1]);\n'
                '  for (unsigned id = 0; id < static_cast<unsigned>(StrId::_COUNT); ++id) {\n'
                '    const char* value = i18n.get(static_cast<StrId>(id));\n'
                '    std::fwrite(value, 1, std::strlen(value) + 1, stdout);\n'
                '  }\n'
                '  return std::strcmp(i18n.get(StrId::_COUNT), "???") != 0;\n'
                '}\n'
            )
            executable = output / "check"
            include_dirs = [
                output, ROOT / "test/language_cache/stubs", ROOT / "lib/I18n",
                ROOT / "lib/LanguageCache", ROOT / "lib/AssetCacheStorage", ROOT / "lib/HalFlashPartition", ROOT / "lib/Memory",
            ]
            subprocess.run(
                ["c++", "-std=c++20", "-DSIMULATOR", "-Wall", "-Wextra", "-Werror",
                 *["-I" + str(path) for path in include_dirs],
                 str(output / "check.cpp"), str(output / "I18nStrings.cpp"),
                 str(ROOT / "lib/I18n/I18n.cpp"),
                 str(ROOT / "lib/LanguageCache/LanguageCache.cpp"),
                 str(ROOT / "lib/AssetCacheStorage/AssetCacheStorage.cpp"), str(ROOT / "lib/HalFlashPartition/HalFlashPartition.cpp"),
                 "-o", str(executable)],
                check=True,
            )
            env = dict(os.environ, CROSSINK_LANGUAGE_FLASH=str(output / "absent-flash.bin"))
            for index, code in enumerate(languages):
                result = subprocess.run([str(executable), code], env=env, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                actual = result.stdout.split(b"\0")[:-1]
                expected_index = index if builtin is None or code in builtin else 0
                expected = [translations[key][expected_index].encode("utf-8") for key in keys]
                self.assertEqual(actual, expected, code)

    def test_all_firmware_strings(self):
        languages, names, keys, translations, inherited = gen_i18n.load_translations(str(ROOT / "lib/I18n/translations"))
        self.assertFalse(any(inherited))
        self.check_runtime(languages, names, keys, translations)

    def test_large_blobs_and_english_fallback(self):
        # Both English and localized offsets cross the old 15-bit boundary.
        languages, names, _, _, _ = gen_i18n.load_translations(str(ROOT / "lib/I18n/translations"))
        keys = ["STR_PADDING", "STR_LOCAL", "STR_SHARED", "STR_FINAL"]
        translations = {
            "STR_PADDING": ["a" * 33000] * len(languages),
            "STR_LOCAL": ["English"] * len(languages),
            "STR_SHARED": ["Same"] * len(languages),
            "STR_FINAL": ["End"] * len(languages),
        }
        russian = languages.index("RU")
        translations["STR_PADDING"][russian] = "б" * 17000
        translations["STR_LOCAL"][russian] = "Локальная строка"
        translations["STR_FINAL"][russian] = "Конец"
        self.check_runtime(languages, names, keys, translations)
        self.check_runtime(languages, names, keys, translations, builtin={"EN"})

    def test_blob_size_boundary(self):
        with tempfile.TemporaryDirectory(prefix="crossink-i18n-limit-") as tmp:
            output = str(Path(tmp) / "I18nStrings.cpp")
            for index in (0, 1):
                translations = {"STR_LIMIT": ["English", "Local"]}
                translations["STR_LIMIT"][index] = "x" * 65534
                gen_i18n.generate_strings_cpp(["EN", "RU"], ["English", "Русский"], ["STR_LIMIT"], translations, output)
                translations["STR_LIMIT"][index] += "x"
                with self.assertRaisesRegex(ValueError, "16-bit blob limit"):
                    gen_i18n.generate_strings_cpp(["EN", "RU"], ["English", "Русский"], ["STR_LIMIT"], translations, output)


if __name__ == "__main__":
    unittest.main()
