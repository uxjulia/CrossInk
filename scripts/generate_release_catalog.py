#!/usr/bin/env python3
"""
Generate public firmware manifests consumed by external apps and OTA.

The catalog follows the simple schema requested by downstream clients and now
emits one entry per firmware device type.
"""

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path


DEVICE_TYPE_ORDER = ('x3-x4', 'sticky', 'x4-pro', 'x4-classic', 'picco', 'eego_a4')
DEFAULT_SUPPORTED_DEVICES = {
    'x3-x4': ['x4', 'x3'],
    'sticky': ['sticky'],
    'x4-pro': ['x4pro'],
    'x4-classic': ['x4c'],
    'picco': ['picco'],
    'eego_a4': ['eego_a4'],
}
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
        help='Path to a firmware .bin artifact. Pass once per firmware device type.',
    )
    parser.add_argument('--output', required=True, type=Path, help='Output catalog path. Use "catalog" for /catalog.')
    parser.add_argument('--repo', required=True, help='GitHub repository in owner/name form.')
    parser.add_argument('--version', required=True, help='Release version, with or without a leading v.')
    parser.add_argument(
        '--firmware-base-url',
        default=None,
        help='Base URL for firmware artifacts. Defaults to the versioned GitHub Release download URL.',
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


def parse_device_type(firmware_path):
    match = FIRMWARE_NAME_PATTERN.match(firmware_path.name)
    if match:
        return match.group('variant')
    return firmware_path.parent.name


def sort_key_for_device_type(device_type):
    try:
        return (DEVICE_TYPE_ORDER.index(device_type), device_type)
    except ValueError:
        return (len(DEVICE_TYPE_ORDER), device_type)


def main():
    args = parse_args()
    version = normalize_version(args.version)
    notes = args.notes or f'CrossInk {version} {args.channel} firmware'
    firmware_base_url = args.firmware_base_url or f'https://github.com/{args.repo}/releases/download/v{version}/'
    firmware_base_url = firmware_base_url.rstrip('/') + '/'

    releases = []
    seen_device_types = set()
    firmware_paths = sorted(args.firmware, key=lambda path: sort_key_for_device_type(parse_device_type(path)))

    for firmware_path in firmware_paths:
        if not firmware_path.is_file():
            raise SystemExit(f'Firmware artifact not found: {firmware_path}')

        filename = firmware_path.name
        device_type = parse_device_type(firmware_path)
        supported_devices = args.supported_devices or DEFAULT_SUPPORTED_DEVICES.get(device_type, [])
        firmware_url = f'{firmware_base_url}{filename}'
        firmware_sha256 = sha256_file(firmware_path)
        firmware_size = firmware_path.stat().st_size
        if device_type in seen_device_types:
            raise SystemExit(f'Duplicate firmware device type supplied: {device_type}')
        seen_device_types.add(device_type)

        releases.append(
            {
                'id': f'{args.channel}-{version}-{device_type}',
                'channel': args.channel,
                'name': version,
                'version': version,
                'variant': device_type,
                'released_at': args.released_at,
                'notes': notes,
                'firmware_url': firmware_url,
                'firmware_sha256': firmware_sha256,
                'size': firmware_size,
                'supported_devices': supported_devices,
            }
        )

    catalog = {
        'schema_version': 1,
        'releases': releases,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf-8')
    print(f'Catalog written to: {args.output}')


if __name__ == '__main__':
    main()
