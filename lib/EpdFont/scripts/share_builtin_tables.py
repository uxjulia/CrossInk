"""Share immutable metadata and byte-identical compressed blocks across built-in faces."""

import argparse
import hashlib
from pathlib import Path
import re
import tempfile

# Match fontconvert.py's static arrays, not arbitrary C++ objects. Keep the
# declared element type and the entire normalized initializer in the key.
TABLE = re.compile(
    r"^static const (EpdUnicodeInterval|EpdLigaturePair|(?:u?int(?:8|16)_t)) "
    r"(\w+(?:Intervals|Kern(?:Left|Right)(?:Codepoints|ClassIds)|"
    r"KernRowOffsets|KernSparse(?:Cols|Values)|LigaturePairs))"
    r"\[\] = \{\n(.*?)^\};",
    re.MULTILINE | re.DOTALL,
)
FACE_INCLUDE = re.compile(r"^#include <builtinFonts/(\w+\.h)>$", re.MULTILINE)
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


BITMAP = re.compile(r"^static const uint8_t (\w+Bitmaps)\[(\d+)\] = \{\n(.*?)^\};", re.M | re.S)
GROUPS = re.compile(r"^static const EpdFontGroup \w+Groups\[\] = \{\n(.*?)^\};", re.M | re.S)
GROUP = re.compile(r"(\s*\{\s*)(\d+)(,\s*(\d+),\s*\d+,\s*\d+,\s*\d+\s*\},\s*)")


GLYPHS = re.compile(r"^static const EpdGlyph (\w+Glyphs)\[\] = \{\n(.*?)^\};", re.M | re.S)
GLYPH_ROW = re.compile(r"\s*\{\s*(-?\d+(?:,\s*-?\d+){6})\s*\},\s*(?://[^\n]*)?")
DESCRIPTOR = re.compile(r"^(static constexpr EpdFontData \w+ = )\{\n(.*?)^\};", re.M | re.S)


def compact_glyphs(name, face):
    glyphs = GLYPHS.search(face)
    if not glyphs:
        return face
    descriptor = DESCRIPTOR.search(face)
    fields = [field.strip() for field in COMMENT.sub("", descriptor[2]).split(",")] if descriptor else []
    if len(fields) < 8 or fields[1] != glyphs[1] or fields[7] not in ("true", "false"):
        raise ValueError(f"{name}: unrecognized built-in font descriptor")
    per_byte = 4 if fields[7] == "true" else 8
    rows = []
    for line in glyphs[2].splitlines():
        match = GLYPH_ROW.fullmatch(line)
        if not match:
            raise ValueError(f"{name}: invalid glyph record: {line}")
        w, h, advance, left, top, length, offset = map(int, match[1].split(","))
        if not (0 <= w <= 255 and 0 <= h <= 255 and 0 <= advance <= 65535
                and -128 <= left <= 127 and -128 <= top <= 127 and 0 <= offset <= 65535
                and length == (w * h + per_byte - 1) // per_byte):
            # Future larger faces can still use the original full-width representation.
            return face
        rows.append(f"    {{ {w}, {h}, {advance}, {left}, {top}, {offset} }},")
    compact = f"static const EpdBuiltinGlyph {glyphs[1]}[] = {{\n" + "\n".join(rows) + "\n};"
    fields_text = descriptor[2].replace(glyphs[1] + ",", "nullptr,", 1)
    wrapped = f"{descriptor[1]}withBuiltinGlyphs({{\n{fields_text}}}, {glyphs[1]});"
    return face.replace(glyphs[0], compact).replace(descriptor[0], wrapped)


def pool_bitmaps(name, face, pool, offsets):
    """Relocate compressed groups; the renderer already reads each by offset and size."""
    groups = GROUPS.search(face)
    if not groups:
        return face
    bitmap = BITMAP.search(face)
    if not bitmap:
        raise ValueError(f"{name}: compressed groups without a bitmap array")
    data = bytes.fromhex(bitmap[3].replace("0x", "").replace(",", " "))
    if len(data) != int(bitmap[2]):
        raise ValueError(f"{name}: invalid bitmap length")

    rows = []
    consumed = 0
    for line in groups[1].splitlines(keepends=True):
        row = GROUP.fullmatch(line)
        if not row:
            raise ValueError(f"{name}: invalid group initializer: {line!r}")
        start, size = int(row[2]), int(row[4])
        if start != consumed or start + size > len(data):
            raise ValueError(f"{name}: invalid compressed group range")
        block = data[start:start + size]
        if block not in offsets:
            offsets[block] = len(pool)
            pool.extend(block)
        rows.append(f"{row[1]}{offsets[block]}{row[3]}")
        consumed += size
    if consumed != len(data):
        raise ValueError(f"{name}: compressed groups do not cover bitmap array")

    # Include the pool only from compressed faces, respecting all_faces.h's profile gate.
    alias = f'#include "BuiltinFontBitmapPool.generated.h"\nstatic constexpr auto& {bitmap[1]} = builtin_font_tables::bitmapPool;'
    return face.replace(groups[0], groups[0].replace(groups[1], "".join(rows))).replace(bitmap[0], alias)


def write_changed(path, text):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, delete=False
        ) as output:
            temp_path = Path(output.name)
            try:
                output.write(text)
                output.close()
                temp_path.replace(path)
            finally:
                temp_path.unlink(missing_ok=True)


def generate(source, output):
    """Generate a profile-local aggregate and return the number of shared tables."""
    manifest = (source / "all_faces.h").read_text(encoding="utf-8")
    names = FACE_INCLUDE.findall(manifest)
    if not names:
        raise ValueError("no built-in font faces in all_faces.h")
    faces = {name: (source / name).read_text(encoding="utf-8") for name in names}
    tables = {}
    keys = {}

    for name, face in faces.items():
        for match in TABLE.finditer(face):
            element_type, symbol, initializer = match.groups()
            normalized = re.sub(r"\s+", "", COMMENT.sub("", initializer))
            key = element_type, normalized
            keys[name, symbol] = key
            count, _ = tables.get(key, (0, initializer))
            tables[key] = count + 1, initializer

    shared = {}
    declarations = []
    for key, (count, initializer) in sorted(tables.items()):
        if count < 2:
            continue
        element_type, normalized = key
        digest = hashlib.sha256(f"{element_type}:{normalized}".encode()).hexdigest()
        symbol = f"table_{digest[:24]}"
        shared[key] = symbol
        declarations.append(f"static const {element_type} {symbol}[] = {{\n{initializer}}};\n")

    output.mkdir(parents=True, exist_ok=True)
    write_changed(
        output / "BuiltinFontTables.generated.h",
        "#pragma once\n#include <EpdFontData.h>\n\n"
        "namespace builtin_font_tables {\n\n"
        + "\n".join(declarations)
        + "\n}  // namespace builtin_font_tables\n",
    )
    pool = bytearray()
    offsets = {}
    for name, face in faces.items():
        def replace(match):
            symbol = match[2]
            shared_symbol = shared.get(keys[name, symbol])
            if shared_symbol is None:
                return match[0]
            return f"static constexpr auto& {symbol} = builtin_font_tables::{shared_symbol};"

        write_changed(output / name, pool_bitmaps(name, compact_glyphs(name, TABLE.sub(replace, face)), pool, offsets))

    if pool:
        rows = (", ".join(map(str, pool[i:i + 32])) for i in range(0, len(pool), 32))
        write_changed(output / "BuiltinFontBitmapPool.generated.h",
                      "#pragma once\n#include <cstdint>\nnamespace builtin_font_tables {\n"
                      "static const uint8_t bitmapPool[] = {\n" + ",\n".join(rows) + "\n};\n}\n")

    # Preserve the manifest's existing scalable/bitmap profile gate. Both
    # common declarations and transformed faces stay private to this include.
    manifest = FACE_INCLUDE.sub(r'#include "\1"', manifest)
    write_changed(
        output / "BuiltinFontsShared.generated.h",
        '#pragma once\n#include "BuiltinFontTables.generated.h"\n'
        + manifest.removeprefix("#pragma once\n"),
    )
    return len(shared)


try:
    Import("env")  # PlatformIO/SCons
except NameError:
    if __name__ == "__main__":
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("source", type=Path)
        parser.add_argument("output", type=Path)
        args = parser.parse_args()
        print(f"Shared {generate(args.source, args.output)} built-in font tables")
else:
    if (env.GetProjectOption("custom_firmware_device_type", "") == "x3-x4"
            or env.GetProjectOption("board_build.mcu", "") == "esp32s3"):
        source = Path(env.subst("$PROJECT_DIR")) / "lib/EpdFont/builtinFonts"
        output = Path(env.subst("$BUILD_DIR")) / "crossink_builtin_fonts"
        count = generate(source, output)
        env.Append(CPPPATH=[str(output)], CPPDEFINES=["CROSSINK_SHARED_BUILTIN_FONT_TABLES"])
        print(f"Shared {count} built-in font tables")
