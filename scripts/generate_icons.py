#!/usr/bin/env python3
"""Generate CrossInk icon headers from a pinned SVG icon library."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


PROJECT_ROOT = Path(__file__).resolve().parent.parent
SDK_GENERATOR = PROJECT_ROOT / "freeink-sdk/libs/assets/Icons/tools/gen_icons.py"
SVG_DIRS = {
    "lucide": PROJECT_ROOT / "freeink-sdk/libs/assets/Icons/lucide/icons",
    "tabler": PROJECT_ROOT / "assets/tabler-icons/icons/outline",
    "tabler-filled": PROJECT_ROOT / "assets/tabler-icons/icons/filled",
}
STROKE_WIDTH_ATTRIBUTE = re.compile(r'(\bstroke-width\s*=\s*")[^"]*(")')


def manifest_sources(path):
    sources = []
    for raw_line in Path(path).read_text().splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        sources.append(line.split("=", 1)[-1].strip())
    return sources


def prepare_stroked_svg_dir(svg_dir, manifest, stroke_width):
    temp_dir = tempfile.TemporaryDirectory(prefix="crossink-icons-")
    try:
        prepared_dir = Path(temp_dir.name)
        value = f"{stroke_width:g}"
        for source_name in manifest_sources(manifest):
            source = svg_dir / f"{source_name}.svg"
            contents = source.read_text()
            contents, replacements = STROKE_WIDTH_ATTRIBUTE.subn(
                rf'\g<1>{value}\g<2>', contents, count=1
            )
            if replacements != 1:
                raise ValueError(f"SVG has no stroke-width attribute: {source}")
            (prepared_dir / source.name).write_text(contents)
        return temp_dir, prepared_dir
    except Exception:
        temp_dir.cleanup()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", choices=SVG_DIRS, required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--sizes", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument(
        "--stroke-width",
        type=float,
        help="Override SVG stroke width while rasterizing this manifest",
    )
    args = parser.parse_args()

    svg_dir = SVG_DIRS[args.library]
    if not SDK_GENERATOR.is_file():
        parser.error(f"FreeInk SDK icon generator is missing: {SDK_GENERATOR}")
    if not svg_dir.is_dir():
        parser.error(f"{args.library.title()} icon source is missing: {svg_dir}")

    output = Path(args.out)
    temp_dir = None
    if args.stroke_width is not None:
        if args.stroke_width <= 0:
            parser.error("--stroke-width must be greater than zero")
        try:
            temp_dir, svg_dir = prepare_stroked_svg_dir(svg_dir, args.manifest, args.stroke_width)
        except (OSError, ValueError) as error:
            parser.error(str(error))

    try:
        subprocess.run(
            [
                sys.executable,
                str(SDK_GENERATOR),
                "--shared",
                "--manifest",
                args.manifest,
                "--svgdir",
                str(svg_dir),
                "--sizes",
                args.sizes,
                "--out",
                str(output),
            ],
            check=True,
        )
    finally:
        if temp_dir is not None:
            temp_dir.cleanup()

    # The SDK generator accepts any single-colour SVGs, but its generated
    # comments currently name Lucide. Keep firmware headers accurate without
    # changing the SDK implementation.
    if args.library in ("tabler", "tabler-filled"):
        generated = output.read_text()
        output.write_text(generated.replace("Lucide", "Tabler").replace("lucide:", "tabler:"))


if __name__ == "__main__":
    main()
