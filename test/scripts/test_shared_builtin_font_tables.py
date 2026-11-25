"""Validate deduplication without the ESP toolchain or font source dependencies."""

import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
import zlib

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
    def test_inter_ui_faces_include_azerbaijani_and_persian_coverage(self):
        for face in ("inter_8_regular", "inter_10_regular", "inter_10_bold", "inter_12_regular", "inter_12_bold"):
            with self.subTest(face=face):
                header = (FONT_DIR / f"{face}.h").read_text()
                intervals = header.split(f"{face}Intervals[] = {{", 1)[1].split("};", 1)[0]
                ranges = [
                    (int(start, 16), int(end, 16))
                    for start, end in re.findall(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),", intervals)
                ]
                for cp in (0x018F, 0x0259, 0x064B, 0x0650, 0x0654, 0x066A):
                    self.assertTrue(any(start <= cp <= end for start, end in ranges), f"missing U+{cp:04X}")

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

            pool_header = (out / "BuiltinFontBitmapPool.generated.h").read_text()
            pool = bytes(map(int, PAYLOAD.search(pool_header)[3].split(",")))
            unique_blocks = set()

            for filename in share.FACE_INCLUDE.findall(source["all_faces.h"]):
                generated = (out / filename).read_text()
                original_matches = {match[2]: match for match in share.TABLE.finditer(source[filename])}

                def undo_alias(match):
                    name, alias = match.groups()
                    kind, initializer = tables[alias]
                    original = original_matches[name]
                    self.assertEqual((kind, normalize(initializer)), (original[1], normalize(original[3])))
                    return original[0]

                original = source[filename]
                # Decode every compact record independently and restore the source
                # declaration before checking the rest of the header byte-for-byte.
                before_glyphs = share.GLYPHS.search(original)
                packed = re.search(r"static const EpdBuiltinGlyph (\w+)\[\] = \{\n(.*?)^\};", generated, re.M | re.S)
                self.assertIsNotNone(packed, filename)
                self.assertEqual(packed[1], before_glyphs[1])
                def records(body):
                    return [tuple(map(int, row.split(",")))
                            for row in re.findall(r"^\s*\{\s*([-\d, ]+)\}", body, re.M)]
                raw = records(before_glyphs[2])
                per_byte = 4 if "    true,\n" in share.DESCRIPTOR.search(original)[2] else 8
                expanded = [(w, h, adv, left, top, (w*h + per_byte - 1)//per_byte, off)
                            for w, h, adv, left, top, off in records(packed[2])]
                self.assertEqual(expanded, raw, filename)
                descriptor = share.DESCRIPTOR.search(original)
                expected = (descriptor[1] + "withBuiltinGlyphs({\n" +
                            descriptor[2].replace(packed[1] + ",", "nullptr,", 1) + "}, " + packed[1] + ");")
                self.assertIn(expected, generated)
                generated = generated.replace(packed[0], before_glyphs[0]).replace(expected, descriptor[0])
                if "static const EpdFontGroup " in original:
                    bitmap = share.BITMAP.search(original)
                    original_data = bytes.fromhex(bitmap[3].replace("0x", "").replace(",", " "))
                    before, after = share.GROUPS.search(original), share.GROUPS.search(generated)
                    def rows(groups):
                        return [tuple(map(int, row.split(",")))
                                for row in re.findall(r"\{([^{}]+)\}", groups[1])]

                    expected_rows, actual_rows = rows(before), rows(after)
                    self.assertEqual(len(actual_rows), len(expected_rows))
                    for (offset, size, expanded, count, first), actual in zip(expected_rows, actual_rows):
                        new_offset, *metadata = actual
                        self.assertEqual(metadata, [size, expanded, count, first])
                        block = original_data[offset:offset + size]
                        unique_blocks.add(block)
                        self.assertEqual(pool[new_offset:new_offset + size], block)
                        self.assertEqual(len(zlib.decompress(block, -15)), expanded)

                    # Restore the two substitutions before checking every other byte.
                    alias = (f'#include "BuiltinFontBitmapPool.generated.h"\n'
                             f'static constexpr auto& {bitmap[1]} = builtin_font_tables::bitmapPool;')
                    generated = generated.replace(alias, bitmap[0]).replace(after[0], before[0])

                self.assertEqual(ALIAS.sub(undo_alias, generated), original)

            self.assertEqual(len(pool), sum(map(len, unique_blocks)))
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

    def test_wide_face_keeps_full_records(self):
        original = (FONT_DIR / "ui_symbols_10.h").read_text()
        for row in ("18, 18, 298, -129, 17, 41, 0", "18, 18, 298, 0, 128, 41, 0",
                    "18, 18, 298, 0, 17, 41, 65536", "18, 18, 298, 0, 17, 42, 0"):
            with self.subTest(row=row):
                wide = original.replace("18, 18, 298, 0, 17, 41, 0", row)
                self.assertNotEqual(wide, original)
                self.assertEqual(share.compact_glyphs("wide", wide), wide)

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
