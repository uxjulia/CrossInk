"""Validate deduplication without the ESP toolchain or font source dependencies."""

import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = ROOT / "lib/EpdFont/builtinFonts"
SCRIPT = ROOT / "lib/EpdFont/scripts/share_builtin_tables.py"
SPEC = importlib.util.spec_from_file_location("share_builtin_tables", SCRIPT)
share = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(share)

ALIAS = re.compile(r"static constexpr auto& (\w+) = builtin_font_tables::(\w+);")
PAYLOAD = re.compile(r"static const (\w+) (\w+)\[\] = \{\n(.*?)^\};", re.MULTILINE | re.DOTALL)


def normalize(initializer):
    return re.sub(r"\s+|//[^\n]*|/\*.*?\*/", "", initializer, flags=re.DOTALL)


class BuiltinFontTablesTest(unittest.TestCase):
    def test_real_header_payloads_and_inputs_are_preserved(self):
        source = {entry.name: entry.read_text() for entry in FONT_DIR.glob("*.h")}
        ids_before = (ROOT / "src/fontIds.h").read_bytes()
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            unique_shared = share.generate(FONT_DIR, out)
            self.assertGreater(unique_shared, 0)
            shared_header = (out / "BuiltinFontTables.generated.h").read_text()
            tables = {
                symbol: (kind, initializer)
                for kind, symbol, initializer in PAYLOAD.findall(shared_header)
            }
            self.assertEqual(unique_shared, len(tables))
            self.assertEqual(len(tables), len({(kind, normalize(value)) for kind, value in tables.values()}))

            for filename in share.FACE_INCLUDE.findall(source["all_faces.h"]):
                generated = (out / filename).read_text()
                original_matches = {match[2]: match for match in share.TABLE.finditer(source[filename])}

                def undo_alias(match):
                    name, alias = match.groups()
                    kind, initializer = tables[alias]
                    original = original_matches[name]
                    self.assertEqual((kind, normalize(initializer)), (original[1], normalize(original[3])))
                    return original[0]

                # Every nonshared byte, including glyphs, bitmaps, counts and
                # names, must stay identical. The only allowed edits are table aliases.
                self.assertEqual(ALIAS.sub(undo_alias, generated), source[filename])

            expected_manifest = source["all_faces.h"].replace("<builtinFonts/", '"').replace(".h>", '.h"')
            aggregate = (out / "BuiltinFontsShared.generated.h").read_text()
            self.assertEqual(
                aggregate.replace('#include "BuiltinFontTables.generated.h"\n', ""),
                expected_manifest,
            )

            mtimes = {entry.name: entry.stat().st_mtime_ns for entry in out.iterdir()}
            self.assertEqual(share.generate(FONT_DIR, out), unique_shared)
            self.assertEqual({entry.name: entry.stat().st_mtime_ns for entry in out.iterdir()}, mtimes)

        self.assertEqual({entry.name: entry.read_text() for entry in FONT_DIR.glob("*.h")}, source)
        self.assertEqual((ROOT / "src/fontIds.h").read_bytes(), ids_before)

    def test_type_is_part_of_identity_and_bitmap_is_not_eligible(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            output = source / "out"
            (source / "all_faces.h").write_text(
                "#pragma once\n#include <builtinFonts/one.h>\n#include <builtinFonts/two.h>\n"
            )
            (source / "one.h").write_text(
                "static const uint8_t oneKernLeftClassIds[] = {\n    0, 1,\n};\n"
                "static const uint16_t oneKernLeftCodepoints[] = {\n    0, 1,\n};\n"
                "static const uint8_t oneBitmaps[] = {\n    0, 1,\n};\n"
            )
            (source / "two.h").write_text(
                "static const uint8_t twoKernLeftClassIds[] = {\n    0,1, // comment\n};\n"
                "static const uint8_t twoBitmaps[] = {\n    0, 1,\n};\n"
            )
            self.assertEqual(share.generate(source, output), 1)
            self.assertIn("static constexpr auto& oneKernLeftClassIds", (output / "one.h").read_text())
            self.assertIn("static constexpr auto& twoKernLeftClassIds", (output / "two.h").read_text())
            self.assertIn("static const uint16_t oneKernLeftCodepoints", (output / "one.h").read_text())
            self.assertIn("static const uint8_t twoBitmaps", (output / "two.h").read_text())


if __name__ == "__main__":
    unittest.main()
