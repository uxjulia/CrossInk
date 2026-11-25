"""
PlatformIO post-build script: fail when firmware.bin does not fit in the
selected application partition, or the smallest app slot for multi-slot layouts.
"""

import csv
import os
import re
import sys


def _parse_size(value):
    value = value.strip()
    match = re.fullmatch(r'(\d+)([KkMm])?', value)
    if match:
        size = int(match.group(1), 10)
        suffix = match.group(2)
        if suffix in ('K', 'k'):
            return size * 1024
        if suffix in ('M', 'm'):
            return size * 1024 * 1024
        return size
    return int(value, 0)


def _get_project_option(env, name):
    try:
        value = env.GetProjectOption(name)
    except Exception:
        return None
    if isinstance(value, str):
        value = value.strip()
    return value or None


def _get_partition_file(project_dir, env):
    partition_file = _get_project_option(env, 'board_build.partitions')
    if not partition_file:
        return None
    if os.path.isabs(partition_file):
        return partition_file
    return os.path.join(project_dir, partition_file)


def _get_app_partition_limit(partition_file, target_label=None):
    app_sizes = []
    app_labels = []

    with open(partition_file, newline='', encoding='utf-8') as fp:
        for row in csv.reader(fp):
            if not row:
                continue

            row = [cell.strip() for cell in row]
            if not row[0] or row[0].startswith('#') or len(row) < 5:
                continue

            name, partition_type, _subtype, _offset, size = row[:5]
            if partition_type != 'app' or (target_label and name != target_label):
                continue

            app_labels.append(name)
            app_sizes.append(_parse_size(size))

    if not app_sizes:
        return None, None

    return min(app_sizes), ', '.join(app_labels)


def check_firmware_size(source, target, env):
    del source

    firmware_path = str(target[0])
    firmware_size = os.path.getsize(firmware_path)
    project_dir = env['PROJECT_DIR']
    partition_file = _get_partition_file(project_dir, env)

    if not partition_file or not os.path.exists(partition_file):
        print('Unable to find partition table for firmware size check', file=sys.stderr)
        env.Exit(1)

    target_label = _get_project_option(env, "custom_firmware_app_partition")
    limit, labels = _get_app_partition_limit(partition_file, target_label)
    if not limit:
        print(f'No app partition found in {partition_file}', file=sys.stderr)
        env.Exit(1)

    remaining = limit - firmware_size
    rel_partition_file = os.path.relpath(partition_file, project_dir)
    if remaining < 0:
        print(
            f'Firmware image is too large for OTA app partition: '
            f'{firmware_size} bytes > {limit} bytes '
            f'({rel_partition_file}: {labels}); over by {-remaining} bytes',
            file=sys.stderr,
        )
        env.Exit(1)

    print(
        f'Firmware image fits OTA app partition: '
        f'{firmware_size} bytes <= {limit} bytes '
        f'({remaining} bytes free)',
    )


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        check_firmware_size,
    )
except NameError:
    print('check_firmware_size.py: must be run via PlatformIO', file=sys.stderr)
