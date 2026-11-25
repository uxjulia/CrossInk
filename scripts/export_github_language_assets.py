#!/usr/bin/env python3
"""Export individual language and hyphenation downloads for crossink-assets."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import publish_language_assets as publisher


def github_files(bundles):
    """GitHub offers the latest files; device catalogs retain their own contracts."""
    files = {}
    for catalog, payloads in bundles:
        publisher.validate_companions(catalog, payloads)
        key = f"catalogs/{catalog['channel']}/{catalog['kind']}.json"
        data = publisher.encode(catalog)
        if len(data) > 8192:
            raise RuntimeError('GitHub catalog exceeds its byte limit')
        files.update(payloads)
        # Inky rechecks the channel and immutable catalog before each download.
        files[f"snapshots/{catalog['revision']}/{key}"] = data
        files[key] = data
    return files


def write_files(output, files):
    # Check every immutable file before changing any channel pointer.
    for name, data in files.items():
        path = output / name
        if name.startswith('snapshots/') and path.exists() and path.read_bytes() != data:
            raise RuntimeError('immutable GitHub snapshot collision: ' + name)
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='crossink-assets assets/ directory')
    release = parser.add_mutually_exclusive_group()
    release.add_argument('--release-tag', help='Published firmware release tag; omit for local fixtures')
    release.add_argument('--prepare-release', help='Prepare downloads before this firmware release is published')
    parser.add_argument('--source-commit', help='Exact full source commit required with --prepare-release')
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--channel', choices=['stable', 'rc'])
    args = parser.parse_args()
    if bool(args.prepare_release) != bool(args.source_commit):
        parser.error('--prepare-release and --source-commit must be used together')
    commit = subprocess.check_output(['git', '-C', str(publisher.root), 'rev-parse', 'HEAD'], text=True).strip()
    version = re.search(r'^version\s*=\s*(\S+)', (publisher.root / 'platformio.ini').read_text(), re.M).group(1)
    channel = args.channel or 'stable'
    if args.release_tag:
        version, channel = publisher.release_identity(args.release_tag, args.repository, commit, args.channel)
    elif args.prepare_release:
        version, channel = publisher.prepared_release_identity(args.prepare_release, args.source_commit, commit,
                                                              args.channel)
    publisher.args = SimpleNamespace(channel=channel, release_tag=args.release_tag, prepare_release=args.prepare_release)
    files = github_files([publisher.build_languages(version, commit), publisher.build_hyphenation()])
    write_files(args.output, files)
    print(f'Exported {len(files)} GitHub files for {channel} to {args.output}; no upload performed')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'Error: {exc}')
