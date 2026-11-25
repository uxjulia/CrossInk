"""Intern byte-identical built-in font tables without changing the source faces.

Only immutable metadata tables are eligible. Glyph and bitmap arrays, font
descriptors, font IDs and downloadable .cpfont records remain untouched.
"""

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
    for name, face in faces.items():
        def replace(match):
            symbol = match[2]
            shared_symbol = shared.get(keys[name, symbol])
            if shared_symbol is None:
                return match[0]
            return f"static constexpr auto& {symbol} = builtin_font_tables::{shared_symbol};"

        write_changed(output / name, TABLE.sub(replace, face))

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
