#!/usr/bin/env python3
"""
Generate the public firmware catalog consumed by external apps.

The catalog intentionally follows the simple schema requested by downstream
clients. CrossInk currently exposes the tiny build as the default firmware.
"""

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


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
    parser.add_argument('--firmware', required=True, type=Path, help='Path to the tiny firmware .bin artifact.')
    parser.add_argument('--output', required=True, type=Path, help='Output catalog path. Use "catalog" for /catalog.')
    parser.add_argument('--repo', required=True, help='GitHub repository in owner/name form.')
    parser.add_argument('--version', required=True, help='Release version, with or without a leading v.')
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


def main():
    args = parse_args()
    firmware_path = args.firmware
    if not firmware_path.is_file():
        raise SystemExit(f'Firmware artifact not found: {firmware_path}')

    version = normalize_version(args.version)
    filename = firmware_path.name
    supported_devices = args.supported_devices or ['x4, x3']
    notes = args.notes or f'CrossInk {version} stable firmware.'

    catalog = {
        'schema_version': 1,
        'releases': [
            {
                'id': f'{args.channel}-{version}',
                'channel': args.channel,
                'name': version,
                'version': version,
                'released_at': args.released_at,
                'notes': notes,
                'firmware_url': f'https://github.com/{args.repo}/releases/latest/download/{filename}',
                'firmware_sha256': sha256_file(firmware_path),
                'size': firmware_path.stat().st_size,
                'supported_devices': supported_devices,
            }
        ],
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf-8')
    print(f'Catalog written to: {args.output}')


if __name__ == '__main__':
    main()
