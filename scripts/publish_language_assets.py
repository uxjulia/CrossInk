#!/usr/bin/env python3
"""Build and optionally publish compatibility-scoped language and shared hyphenation assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import quote

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))

def parse_args():
    parser = argparse.ArgumentParser(description='Build local translation and hyphenation downloads; upload to S3 only with --publish. No release, commit, or push.')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--publish', action='store_true', help='Upload missing files, verify them, then update selected catalogs')
    mode.add_argument('--check', action='store_true', help='Build and compare against S3 without uploading')
    parser.add_argument('--assets', choices=['both', 'languages', 'hyphenation'], default='both',
                        help='Which assets to build/check/publish (default: both)')
    parser.add_argument('--channel', choices=['stable', 'rc'], help='Defaults to rc with CROSSINK_RC_HASH or version ending in -RC, otherwise stable')
    parser.add_argument('--bucket', default=os.environ.get('LANGUAGE_S3_BUCKET', 'crossink-languages'))
    parser.add_argument('--region', default=os.environ.get('AWS_REGION', 'us-east-1'))
    parser.add_argument('--prefix', default='v1')
    release = parser.add_mutually_exclusive_group()
    release.add_argument('--release-tag', help='Build from this published GitHub release and its exact checked-out tag')
    release.add_argument('--prepare-release', help='Prepare downloads before this firmware release is published')
    parser.add_argument('--source-commit', help='Exact full source commit required with --prepare-release')
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY'), help='GitHub owner/repository for --release-tag')
    args = parser.parse_args()
    if bool(args.prepare_release) != bool(args.source_commit):
        parser.error('--prepare-release and --source-commit must be used together')
    if not re.fullmatch(r'[a-z0-9][a-z0-9.-]+', args.bucket) or not re.fullmatch(r'[a-z0-9-]+', args.region):
        parser.error('invalid bucket or region')
    if not re.fullmatch(r'[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*', args.prefix):
        parser.error('prefix must be nonempty path components without dots')

    return args


def tag_identity(tag, channel=None):
    is_rc = bool(re.fullmatch(r'rc-[A-Za-z0-9._-]+-[0-9a-f]{7}', tag))
    if not is_rc and not re.fullmatch(r'v[0-9]+(?:\.[0-9]+){2,3}', tag):
        raise RuntimeError('unsupported firmware release tag: ' + tag)
    expected_channel = 'rc' if is_rc else 'stable'
    if channel is not None and channel != expected_channel:
        raise RuntimeError('release tag does not match requested channel')
    # Match package_languages.py in the RC and stable release workflows.
    return (tag if is_rc else tag[1:]), expected_channel


def require_clean_source():
    dirty = subprocess.check_output(
        ['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=no'], text=True).strip()
    if dirty:
        raise RuntimeError('release publication requires clean tracked source files and submodules')


def prepared_release_identity(tag, source_commit, commit, channel=None):
    """Validate a release build before its tag or GitHub draft exists."""
    identity = tag_identity(tag, channel)
    if not re.fullmatch(r'[0-9a-f]{40}', source_commit or '') or source_commit != commit:
        raise RuntimeError('preparing a release requires its exact full source commit')
    if identity[1] == 'rc' and not tag.endswith('-' + commit[:7]):
        raise RuntimeError('release candidate tag does not match source commit')
    require_clean_source()
    # A retry may already have a tag; never prepare another commit under it.
    ref = subprocess.run(['git', '-C', str(root), 'rev-parse', '--verify', '--quiet',
                          f'refs/tags/{tag}^{{commit}}'], capture_output=True, text=True)
    if ref.returncode == 0:
        if ref.stdout.strip() != commit:
            raise RuntimeError('checked-out source does not match existing release tag')
    elif ref.returncode != 1:
        raise RuntimeError('could not verify release tag: ' + ref.stderr.strip())
    return identity


def release_identity(tag, repository, commit, channel=None):
    """Fail before S3 access unless this is the clean source of a published release."""
    if not repository or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise RuntimeError('--release-tag requires a GitHub owner/repository')
    identity = tag_identity(tag, channel)
    tag_commit = subprocess.check_output(
        ['git', '-C', str(root), 'rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}'], text=True).strip()
    if tag_commit != commit:
        raise RuntimeError('checked-out source does not match release tag')
    require_clean_source()
    release = json.loads(subprocess.check_output(
        ['gh', 'api', f'repos/{repository}/releases/tags/{quote(tag, safe="")}'], text=True))
    if (release.get('tag_name') != tag or release.get('draft') is not False
            or not release.get('published_at') or release.get('prerelease') is not (identity[1] == 'rc')):
        raise RuntimeError('release must be published and its prerelease flag must match the tag')
    return identity


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def aws(*command):
    result = subprocess.run(['aws', '--region', args.region, '--no-cli-pager', '--cli-connect-timeout', '10', '--cli-read-timeout', '60', 's3api', *command],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout or '{}')


def remote_get(key, destination, missing_ok=False):
    try:
        meta = aws('get-object', '--bucket', args.bucket, '--key', args.prefix + '/' + key, str(destination))
        return destination.read_bytes(), meta['ETag']
    except RuntimeError as exc:
        if missing_ok and '(NoSuchKey)' in str(exc):
            return None, None
        raise


def validate_companions(catalog, files):
    base = 'snapshots/' + catalog['revision'] + '/'
    for entry in catalog['entries']:
        selected = []
        for index in ('notices.json', 'sources.json'):
            data = files[base + index]
            if len(data) > 2097152:
                raise RuntimeError('companion index exceeds device limit')
            refs = json.loads(data)[catalog['kind']][entry['code']]['files']
            if not 0 < len(refs) <= 16:
                raise RuntimeError('invalid companion reference count')
            selected.extend(refs)
            for ref in refs:
                path = ref['path']
                if (not path.startswith(base + 'source/') or len(path.encode('ascii')) > 240
                        or any(part in ('', '.', '..') or part.endswith(('.', ' ')) for part in path.split('/'))):
                    raise RuntimeError('invalid companion path: ' + path)
                body = files[path]
                if not 0 < len(body) <= 2097152 or len(body) != ref['bytes'] or sha(body) != ref['sha256']:
                    raise RuntimeError('invalid companion file: ' + path)
        if sum(ref['bytes'] for ref in selected) > 8388608:
            raise RuntimeError('companion closure exceeds device limit')


def build_languages(version, commit):
    from generate_language_template import generate
    prepared = bool(getattr(args, 'prepare_release', None))
    released = bool(args.release_tag or prepared)
    # Preparing and retrying after publication must produce identical snapshots.
    provenance = 'release-build' if released else 'local-working-files'
    artifacts = generate(version if released else version + '-local', commit,
                         'release-build' if released else 'local-fixture')
    template = json.loads(artifacts['language-template.json'])
    sources = {str(p.relative_to(root)): p.read_bytes() for p in sorted((root / 'lib/I18n/translations').glob('*.yaml'))}
    for name in ('LICENSE', 'lib/I18n/languages.json', 'scripts/gen_i18n.py', 'scripts/generate_language_template.py', 'scripts/publish_language_assets.py'):
        sources[name] = (root / name).read_bytes()
    identity = encode({name: sha(data) for name, data in sorted({**sources, **artifacts}.items())})
    # Content identity prevents uncommitted edits from overwriting an earlier snapshot.
    revision = hashlib.sha256(b'crossink-local-languages-v1\n' + identity).hexdigest()[:40]
    base = f'snapshots/{revision}'
    files = {}

    def put(name, data):
        key = base + '/' + name
        files[key] = data
        return {'path': key, 'bytes': len(data), 'sha256': sha(data)}

    refs = {name: put('source/firmware/' + name, data) for name, data in sources.items()}
    for name, data in artifacts.items():
        put('editor/' + name, data)
    notices, companions, entries = {}, {}, []
    common = [refs[name] for name in ('scripts/gen_i18n.py', 'scripts/generate_language_template.py',
                                     'lib/I18n/languages.json', 'lib/I18n/translations/english.yaml')]
    from gen_i18n import parse_yaml_file
    originals = {parse_yaml_file(str(root / name)).get('_language_code'): name
                 for name in sources if name.endswith('.yaml')}
    for item in template['languages']:
        if 'translation' not in item:
            continue
        ref = item['translation']
        data = artifacts[ref['filename']]
        if sha(data) != ref['sha256']:
            raise RuntimeError('translation checksum mismatch')
        put('translation/' + ref['sha256'] + '.yaml', data)
        code = item['code']
        entries.append({'code': code, 'name': item['name'], 'bytes': len(data), 'sha256': sha(data)})
        notices[code] = {'license': 'MIT', 'files': [refs['LICENSE']]}
        companions[code] = {'revision': commit, 'provenance': provenance,
                            'files': [refs[originals[code]], *common]}
    put('notices.json', encode({'translation': notices}))
    put('sources.json', encode({'translation': companions}))
    put('snapshot.json', encode({'revision': revision, 'sourceCommit': commit, 'status': provenance,
                                'firmwareVersion': version, 'files': {k: sha(v) for k, v in files.items()}}))
    catalog = {'schema': 1, 'kind': 'translation', 'channel': args.channel, 'revision': revision,
               'format': 1, 'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json',
               'source': template['englishSourceSha256'], 'keyset': template['keysetSha256'],
               'entries': sorted(entries, key=lambda e: e['code'])}
    return catalog, files


def build_hyphenation():
    # Separate content identity: translation/firmware edits must not republish
    # unchanged dictionaries. Include the exact generator, tables and notices.
    from package_hyphenation import artifacts as hyphenation_artifacts, CODES, PACK_HEADER
    import zlib
    packed = hyphenation_artifacts(root)
    hfiles = {k.replace('hyphenation-sources/', 'source/hypher/'): v for k, v in packed.items()}
    for name in ('scripts/package_hyphenation.py', 'scripts/generate_hyphenation_trie.py'):
        hfiles['source/firmware/' + name] = (root / name).read_bytes()
    for code in CODES:
        name = f'lib/Epub/Epub/hyphenation/generated/hyph-{code}.trie.h'
        hfiles['source/firmware/' + name] = (root / name).read_bytes()
    hrevision = sha(b'crossink-hyphenation-v3\n' + encode({k: sha(v) for k, v in sorted(hfiles.items())}))[:40]
    hbase = f'snapshots/{hrevision}'
    hentries, hnotices, hsources = [], {}, {}
    def href(name):
        data = hfiles[name]
        return {'path': hbase + '/' + name, 'bytes': len(data), 'sha256': sha(data)}
    for code in CODES:
        name = f'.crossink/hyphenation/hyph-{code}.cphyph'
        data = hfiles[name]
        magic, fmt, lang, left, right, flags, reserved, offset, size, crc = PACK_HEADER.unpack_from(data)
        payload = data[PACK_HEADER.size:]
        if (magic != b'CPHY' or fmt != 1 or lang.decode() != code or flags or reserved
                or left != 2 or right != 2 or size != len(payload) or offset >= size
                or zlib.crc32(payload) != crc or len(data) > 262144):
            raise RuntimeError('invalid hyphenation pack: ' + code)
        hfiles['hyphenation/' + sha(data) + '.cphyph'] = data
        hentries.append({'code': code, 'bytes': len(data), 'sha256': sha(data)})
        pattern = 'source/hypher/patterns/hyph-' + ('de-1996' if code == 'de' else code) + '.tex'
        # The pattern's own license is authoritative, not Hypher's MIT license.
        hnotices[code] = {'license': 'See per-pattern terms', 'licenses': ['See per-pattern terms'],
            'licenseDeclaration': 'Original authorship and license terms are preserved in the included pattern source.',
            'files': [href(pattern),
            href('source/hypher/README.md'), href('source/hypher/LICENSE-MIT'),
            href('source/hypher/LICENSE-APACHE')]}
        hsources[code] = {'provenance': 'source-snapshot', 'files': [href(pattern),
            href(f'source/firmware/lib/Epub/Epub/hyphenation/generated/hyph-{code}.trie.h'),
            href('source/firmware/scripts/package_hyphenation.py'),
            href('source/firmware/scripts/generate_hyphenation_trie.py')]}
    hfiles['notices.json'] = encode({'hyphenation': hnotices})
    hfiles['sources.json'] = encode({'hyphenation': hsources})
    hfiles['snapshot.json'] = encode({'revision': hrevision, 'status': 'source-snapshot',
                                    'files': {hbase + '/' + k: sha(v) for k, v in hfiles.items()}})
    hcatalog = {'schema': 1, 'kind': 'hyphenation', 'channel': args.channel, 'revision': hrevision,
                'format': 1, 'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json',
                'entries': sorted(hentries, key=lambda e: e['code'])}
    return hcatalog, {hbase + '/' + k: v for k, v in hfiles.items()}


def catalog_key(catalog):
    kind, channel = catalog.get('kind'), catalog.get('channel')
    if (kind not in ('translation', 'hyphenation') or channel not in ('stable', 'rc')
            or catalog.get('schema') != 1 or catalog.get('format') != 1 or catalog.get('requires') != 0
            or not re.fullmatch(r'[0-9a-f]{40}', catalog.get('revision', ''))):
        raise RuntimeError('invalid catalog identity')
    if kind == 'translation':
        if any(not re.fullmatch(r'[0-9a-f]{64}', catalog.get(k, '')) for k in ('source', 'keyset')):
            raise RuntimeError('invalid translation contract')
        return f"catalogs/{channel}/translation/{catalog['source']}/{catalog['keyset']}.json"
    return f'catalogs/{channel}/hyphenation.json'


def previous_entries(catalog, previous):
    if previous is None:
        return
    if catalog_key(previous) != catalog_key(catalog):
        raise RuntimeError('existing catalog has a different identity')
    prior = {entry['code']: entry for entry in previous['entries']}
    for entry in catalog['entries']:
        old = prior.get(entry['code'], {})
        value = old.get('sha256') if old.get('sha256') != entry['sha256'] else old.get('previous')
        if value:
            if not re.fullmatch(r'[0-9a-f]{64}', value) or value == entry['sha256']:
                raise RuntimeError('invalid previous checksum')
            entry['previous'] = value


def main():
    global args
    args = parse_args()
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    version = re.search(r'^version\s*=\s*(\S+)', (root / 'platformio.ini').read_text(), re.M).group(1)
    if args.release_tag:
        version, args.channel = release_identity(args.release_tag, args.repository, commit, args.channel)
    elif args.prepare_release:
        version, args.channel = prepared_release_identity(args.prepare_release, args.source_commit, commit, args.channel)
    elif args.channel is None:
        args.channel = 'rc' if os.environ.get('CROSSINK_RC_HASH') or version.endswith('-RC') else 'stable'
    bundles = []
    if args.assets in ('both', 'languages'):
        bundles.append(build_languages(version, commit))
    if args.assets in ('both', 'hyphenation'):
        bundles.append(build_hyphenation())
    output = root / '.local-language-publish' / args.channel
    output.mkdir(parents=True, exist_ok=True)
    print(f'Target: s3://{args.bucket}/{args.prefix}/catalogs/{args.channel}/', flush=True)
    summary = ', '.join(f"{len(catalog['entries'])} {catalog['kind']} entries" for catalog, _ in bundles)
    print(f'Built {summary}; firmware {version}', flush=True)
    with tempfile.TemporaryDirectory(prefix='crossink-language-upload-') as scratch:
        scratch = Path(scratch)
        plans, promotions = [], []

        def existing(key):
            data, etag = remote_get(key, scratch / 'old.json', True) if args.publish or args.check else (None, None)
            value = json.loads(data) if data is not None else None
            if value is not None:
                if not isinstance(value, dict):
                    raise RuntimeError('invalid existing catalog: ' + key)
                identity = catalog_key(value)
                legacy = f"catalogs/{value['channel']}/translation.json"
                if key != identity and not (value['kind'] == 'translation' and key == legacy):
                    raise RuntimeError('existing catalog has wrong identity: ' + key)
            return data, etag, value

        def promote(key, data, old, etag):
            dest = output / key
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            promotions.append((key, data, old, etag))

        # Preflight every selected catalog and immutable object before remote writes.
        for catalog, files in bundles:
            validate_companions(catalog, files)
            kind = catalog['kind']
            key = catalog_key(catalog)
            old, etag, previous = existing(key)
            legacy_key = f'catalogs/{args.channel}/translation.json'
            legacy_data = legacy_etag = legacy = None
            legacy_compatible = False
            if kind == 'translation' and (args.publish or args.check):
                legacy_data, legacy_etag, legacy = existing(legacy_key)
                if legacy is not None:
                    legacy_contract_key = catalog_key(legacy)
                    if legacy_contract_key == key:
                        # Legacy consumers reject codes added after their firmware shipped.
                        legacy_codes = {entry['code'] for entry in legacy['entries']}
                        legacy_compatible = all(entry['code'] in legacy_codes for entry in catalog['entries'])
                        if not legacy_compatible:
                            print('Preserving legacy catalog language codes: ' + legacy_key, flush=True)
                        # Keep update recognition when migrating from the unscoped publisher.
                        if previous is None:
                            previous = legacy
                    else:
                        # Preserve shipped firmware's URL. Seed its scoped path once,
                        # without replacing a newer catalog already published there.
                        scoped_data, scoped_etag, _ = existing(legacy_contract_key)
                        if scoped_data is None:
                            promote(legacy_contract_key, legacy_data, None, scoped_etag)
                        print('Preserving legacy translation catalog: ' + legacy_key, flush=True)
            previous_entries(catalog, previous)
            catalog_data = encode(catalog)
            limit = 9 if kind == 'hyphenation' else 48
            if not 0 < len(catalog['entries']) <= limit or len(catalog_data) > 8192:
                raise RuntimeError(kind + ' catalog exceeds device limits')
            promote(key, catalog_data, old, etag)
            # The first legacy contract and code set stay pinned; compatible corrections may update it.
            if kind == 'translation' and (args.publish or args.check) and (legacy is None or legacy_compatible):
                promote(legacy_key, catalog_data, legacy_data, legacy_etag)
            for name, data in files.items():
                dest = output / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
            missing = []
            if args.publish or args.check:
                for name, data in files.items():
                    remote, _ = remote_get(name, scratch / 'check', True)
                    if remote is not None and remote != data:
                        raise RuntimeError('immutable snapshot collision: ' + name)
                    if remote is None:
                        missing.append(name)
                print(f'{kind}: {len(missing)} missing objects; catalog ' + ('unchanged' if old == catalog_data else 'will update'), flush=True)
            plans.append((files, missing))
        print(f'Local bundle: {output}', flush=True)
        if not args.publish:
            print('No uploads. Add --publish to upload; --check compares against S3 read-only.')
            return
        # Upload and read back every missing object before exposing any selected catalog.
        for files, missing in plans:
            for name in missing:
                content_type = ('application/octet-stream' if name.endswith('.cphyph') else
                                'application/json' if name.endswith('.json') else 'text/plain; charset=utf-8')
                aws('put-object', '--bucket', args.bucket, '--key', args.prefix + '/' + name,
                    '--body', str(output / name), '--if-none-match', '*',
                    '--content-type', content_type, '--cache-control', 'public,max-age=31536000,immutable')
                if remote_get(name, scratch / 'check')[0] != files[name]:
                    raise RuntimeError('S3 read-back mismatch: ' + name)
        # Separate compare-and-swap writes are retryable, including legacy migration.
        for key, data, old, etag in promotions:
            if old != data:
                aws('put-object', '--bucket', args.bucket, '--key', args.prefix + '/' + key,
                    '--body', str(output / key), '--content-type', 'application/json', '--cache-control', 'no-cache',
                    *(['--if-match', etag] if etag else ['--if-none-match', '*']))
            if remote_get(key, scratch / 'check')[0] != data:
                raise RuntimeError('catalog read-back mismatch; another publisher may have updated it')
            print('Verified ' + key, flush=True)
        print(f'Published and verified {args.assets} assets. No release created.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'Error: {exc}')
