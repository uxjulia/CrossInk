#!/usr/bin/env python3
"""
Generate public firmware manifests consumed by external apps and OTA.

The catalog follows the simple schema requested by downstream clients and now
emits one entry per firmware build variant.
"""

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path


VARIANT_ORDER = ('teensy', 'tiny', 'xlarge', 'no_emoji')
FIRMWARE_NAME_PATTERN = re.compile(r'^firmware-(?P<variant>.+?)-v[^/]+\.bin$')


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as firmware:
        for chunk in iter(lambda: firmware.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now_iso():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace('+00:00', 'Z')


def normalize_version(version):
    version = version.strip()
    return version[1:] if version.startswith('v') else version


def parse_args():
    parser = argparse.ArgumentParser(description='Generate CrossInk release catalog JSON.')
    parser.add_argument(
        '--firmware',
        required=True,
        action='append',
        type=Path,
        help='Path to a firmware .bin artifact. Pass once per build variant.',
    )
    parser.add_argument('--output', required=True, type=Path, help='Output catalog path. Use "catalog" for /catalog.')
    parser.add_argument(
        '--ota-output',
        type=Path,
        help='Optional GitHub-release-shaped OTA manifest path consumed by firmware.',
    )
    parser.add_argument('--repo', required=True, help='GitHub repository in owner/name form.')
    parser.add_argument('--version', required=True, help='Release version, with or without a leading v.')
    parser.add_argument(
        '--firmware-base-url',
        default=None,
        help='Base URL for firmware artifacts. Defaults to GitHub Releases latest/download.',
    )
    parser.add_argument('--released-at', default=utc_now_iso(), help='Release timestamp in ISO-8601 format.')
    parser.add_argument('--channel', default='stable', help='Release channel.')
    parser.add_argument('--notes', default=None, help='Free-text changelog shown to users.')
    parser.add_argument(
        '--supported-device',
        action='append',
        dest='supported_devices',
        default=[],
        help='Supported device id. Can be passed more than once.',
    )
    return parser.parse_args()


def parse_variant(firmware_path):
    match = FIRMWARE_NAME_PATTERN.match(firmware_path.name)
    if match:
        return match.group('variant')
    return firmware_path.parent.name


def sort_key_for_variant(variant):
    try:
        return (VARIANT_ORDER.index(variant), variant)
    except ValueError:
        return (len(VARIANT_ORDER), variant)


def main():
    args = parse_args()
    version = normalize_version(args.version)
    supported_devices = args.supported_devices or ['x4', 'x3']
    notes = args.notes or f'CrossInk {version} {args.channel} firmware'
    firmware_base_url = args.firmware_base_url or f'https://github.com/{args.repo}/releases/latest/download/'
    firmware_base_url = firmware_base_url.rstrip('/') + '/'

    releases = []
    ota_assets = []
    seen_variants = set()
    firmware_paths = sorted(args.firmware, key=lambda path: sort_key_for_variant(parse_variant(path)))

    for firmware_path in firmware_paths:
        if not firmware_path.is_file():
            raise SystemExit(f'Firmware artifact not found: {firmware_path}')

        filename = firmware_path.name
        variant = parse_variant(firmware_path)
        firmware_url = f'{firmware_base_url}{filename}'
        firmware_sha256 = sha256_file(firmware_path)
        firmware_size = firmware_path.stat().st_size
        if variant in seen_variants:
            raise SystemExit(f'Duplicate firmware variant supplied: {variant}')
        seen_variants.add(variant)

        releases.append(
            {
                'id': f'{args.channel}-{version}-{variant}',
                'channel': args.channel,
                'name': version,
                'version': version,
                'variant': variant,
                'released_at': args.released_at,
                'notes': notes,
                'firmware_url': firmware_url,
                'firmware_sha256': firmware_sha256,
                'size': firmware_size,
                'supported_devices': supported_devices,
            }
        )
        ota_assets.append(
            {
                'name': filename,
                'browser_download_url': firmware_url,
                'size': firmware_size,
            }
        )

    catalog = {
        'schema_version': 1,
        'releases': releases,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf-8')
    print(f'Catalog written to: {args.output}')

    if args.ota_output:
        ota_manifest = {
            'tag_name': f'v{version}',
            'assets': ota_assets,
        }
        args.ota_output.parent.mkdir(parents=True, exist_ok=True)
        args.ota_output.write_text(json.dumps(ota_manifest, indent=2) + '\n', encoding='utf-8')
        print(f'OTA manifest written to: {args.ota_output}')


if __name__ == '__main__':
    main()
