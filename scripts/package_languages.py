#!/usr/bin/env python3
"""Package editable SD translations and the matching English template for a release."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from generate_language_template import generate, write_artifacts
from package_hyphenation import artifacts as hyphenation_artifacts

ROOT = Path(__file__).resolve().parents[1]


def package(output: Path, version: str, benchmark: bool = False, template_output_dir=None,
            source_commit=None, status="local-fixture"):
    artifacts = generate(version, source_commit, status)
    entries = dict(artifacts)
    entries.update(hyphenation_artifacts())
    # Keep the established archive spelling as an identical compatibility alias.
    entries["english-template.yaml"] = artifacts["english_template.yaml"]
    if template_output_dir is not None:
        write_artifacts(template_output_dir, artifacts)
    source = ROOT / "lib/I18n/translations"
    prefix = f"_firmware_version: {json.dumps(version, ensure_ascii=False)}\n"
    for path in sorted(source.glob("*.yaml")):
        if path.name == "english.yaml":
            continue
        destination = f".crossink/languages/{path.name}"
        entries[destination] = (prefix + path.read_text(encoding="utf-8")).encode("utf-8")
    if benchmark:
        english = (source / "english.yaml").read_text(encoding="utf-8")
        english = english.replace('_language_code: "EN"', '_language_code: "BENCH"\n_keyboard: "EN"')
        english = english.replace('_language_name: "English"', '_language_name: "English benchmark"')
        entries[".crossink/languages/benchmark.yaml"] = (prefix + english).encode("utf-8")
    entries["README.md"] = (ROOT / "docs/languages.md").read_bytes()
    entries["manifest.json"] = (json.dumps({
        "firmware": version,
        "format": "CrossInk flat YAML v1",
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())},
    }, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)
    print(f"Packaged {len(entries)} files: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--benchmark", action="store_true", help="Include the optional English timing fixture")
    parser.add_argument("--template-output-dir", type=Path, help="Also write the atomic editor JSON and YAML pair")
    parser.add_argument("--source-commit", help="Full source snapshot Git SHA")
    parser.add_argument("--status", choices=["local-fixture", "release-build"], default="local-fixture")
    args = parser.parse_args()
    package(args.output, args.version, args.benchmark, args.template_output_dir, args.source_commit, args.status)
