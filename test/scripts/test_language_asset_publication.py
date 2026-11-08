#!/usr/bin/env python3
"""Exercise catalog coexistence and publication ordering with an in-memory S3."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publisher', ROOT / 'scripts/publish_language_assets.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)
import export_github_language_assets as github_export


def bundle(source='a', keyset='b', revision='c', text=b'French', channel='stable', kind='translation'):
    prefix = f'snapshots/{revision * 40}/'
    notice = b'Pattern source and notices'
    ref = {'path': prefix + 'source/notice.txt', 'bytes': len(notice), 'sha256': publisher.sha(notice)}
    code = 'FR' if kind == 'translation' else 'fr'
    entry = {'code': code, 'bytes': len(text), 'sha256': publisher.sha(text)}
    catalog = {'schema': 1, 'kind': kind, 'channel': channel, 'revision': revision * 40,
               'format': 1, 'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json', 'entries': [entry]}
    if kind == 'translation':
        catalog.update(source=source * 64, keyset=keyset * 64)
        entry['name'] = 'French'
    extension = 'yaml' if kind == 'translation' else 'cphyph'
    files = {ref['path']: notice, prefix + kind + '/' + entry['sha256'] + '.' + extension: text}
    for name in ('notices.json', 'sources.json'):
        files[prefix + name] = publisher.encode({kind: {code: {'files': [ref]}}})
    return catalog, files


class PublicationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'platformio.ini').write_text('[crossink]\nversion = 2.0.0\n')
        self.store = {}
        self.writes = []
        self.reads = []
        self.current = bundle()
        self.hyphenation = bundle(kind='hyphenation', revision='f')
        self.fail_key = None
        self.corrupt_key = None

    def aws(self, *command):
        key = command[command.index('--key') + 1].removeprefix('v1/')
        if command[0] == 'get-object':
            self.reads.append(key)
            if key not in self.store:
                raise RuntimeError('(NoSuchKey)')
            data = self.store[key]
            Path(command[-1]).write_bytes(data)
            return {'ETag': publisher.sha(data)}
        self.assertEqual(command[0], 'put-object')
        if key == self.fail_key:
            raise RuntimeError('injected publication failure')
        if '--if-none-match' in command:
            self.assertNotIn(key, self.store, 'create must not overwrite an existing object')
        else:
            self.assertIn('--if-match', command)
            self.assertEqual(command[command.index('--if-match') + 1], publisher.sha(self.store[key]))
        # Every new catalog must have its payload and both indexes already readable.
        data = Path(command[command.index('--body') + 1]).read_bytes()
        if key.startswith('catalogs/'):
            catalog = json.loads(data)
            prefix = f"snapshots/{catalog['revision']}/"
            for name in ('notices.json', 'sources.json'):
                self.assertIn(prefix + name, self.store)
            extension = 'yaml' if catalog['kind'] == 'translation' else 'cphyph'
            for entry in catalog['entries']:
                self.assertIn(prefix + catalog['kind'] + '/' + entry['sha256'] + '.' + extension, self.store)
        self.store[key] = b'corrupt' if key == self.corrupt_key else data
        self.writes.append(key)
        return {}

    def run_publisher(self, *options, clean=False):
        with patch.object(publisher, 'root', self.root), \
             patch.object(publisher, 'aws', self.aws), \
             patch.object(publisher, 'build_languages', side_effect=lambda *_: copy.deepcopy(self.current)), \
             patch.object(publisher, 'build_hyphenation', side_effect=lambda: copy.deepcopy(self.hyphenation)), \
             patch.object(publisher.subprocess, 'check_output', side_effect=lambda command, **_:
                          '' if clean and 'status' in command else '1' * 40), \
             patch.object(publisher.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', '')), \
             patch.dict('os.environ', {}, clear=True), \
             patch.object(sys, 'argv', ['publish_language_assets.py', '--assets', 'languages', *options]), \
             contextlib.redirect_stdout(io.StringIO()):
            publisher.main()

    def seed(self, data, legacy=False):
        catalog, files = data
        self.store.update(files)
        key = (f"catalogs/{catalog['channel']}/translation.json" if legacy else publisher.catalog_key(catalog))
        self.store[key] = publisher.encode(catalog)
        return key

    def test_contracts_coexist_and_legacy_keeps_first_contract(self):
        self.run_publisher('--publish')
        original = dict(self.store)
        self.current = bundle(source='d', revision='d')  # English changes, same keys.
        self.run_publisher('--publish')
        second = publisher.catalog_key(self.current[0])
        self.current = bundle(source='d', keyset='e', revision='e')  # Keyset changes too.
        self.run_publisher('--publish')
        for key, data in original.items():
            self.assertEqual(self.store[key], data)
        self.assertIn(second, self.store)
        self.assertIn(publisher.catalog_key(self.current[0]), self.store)
        self.assertEqual(len([key for key in self.store if key.endswith('.json') and '/translation/' in key]), 3)

    def test_new_languages_update_scoped_catalog_without_breaking_legacy(self):
        self.run_publisher('--publish')
        legacy_key = 'catalogs/stable/translation.json'
        legacy_data = self.store[legacy_key]
        self.current = bundle(revision='d', text=b'Corrected French')
        catalog, files = self.current
        extra = dict(catalog['entries'][0], code='NEW', name='New language')
        catalog['entries'].append(extra)
        for path in list(files):
            if path.endswith(('notices.json', 'sources.json')):
                index = json.loads(files[path])
                index['translation']['NEW'] = copy.deepcopy(index['translation']['FR'])
                files[path] = publisher.encode(index)
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy_key], legacy_data)
        scoped = json.loads(self.store[publisher.catalog_key(catalog)])
        self.assertEqual([entry['code'] for entry in scoped['entries']], ['FR', 'NEW'])
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy_key], legacy_data)

    def test_migrates_legacy_to_scoped_catalog_before_new_contract(self):
        legacy = self.seed(self.current, legacy=True)
        old_data = self.store[legacy]
        old_key = publisher.catalog_key(self.current[0])
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy], old_data)
        self.assertEqual(self.store[old_key], old_data)
        self.assertIn(publisher.catalog_key(self.current[0]), self.store)

    def test_migration_never_replaces_newer_scoped_catalog(self):
        self.seed(self.current, legacy=True)
        updated = bundle(revision='e', text=b'Updated French')
        key = self.seed(updated)
        expected = self.store[key]
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--publish')
        self.assertEqual(self.store[key], expected)

    def test_compatible_corrections_update_legacy_and_remember_previous_hash(self):
        self.seed(self.current, legacy=True)
        old_digest = self.current[0]['entries'][0]['sha256']
        self.current = bundle(revision='d', text=b'Corrected French')
        self.run_publisher('--publish')
        key = publisher.catalog_key(self.current[0])
        catalog = json.loads(self.store[key])
        self.assertEqual(catalog['entries'][0]['previous'], old_digest)
        self.assertEqual(self.store[key], self.store['catalogs/stable/translation.json'])
        count = len(self.writes)
        self.run_publisher('--publish')
        self.assertEqual(len(self.writes), count)

    def test_rc_and_hyphenation_are_independent(self):
        self.run_publisher('--publish', '--assets', 'both')
        stable = dict(self.store)
        self.current = bundle(source='d', channel='rc', revision='d')
        self.run_publisher('--publish', '--channel', 'rc')
        for key, data in stable.items():
            self.assertEqual(self.store[key], data)
        self.assertIn('catalogs/rc/translation.json', self.store)
        self.assertIn('catalogs/stable/hyphenation.json', self.store)
        self.assertNotIn('catalogs/rc/hyphenation.json', self.store)

    def test_build_is_offline_and_check_is_read_only(self):
        self.run_publisher()
        self.assertEqual(self.reads, [])
        self.assertEqual(self.writes, [])
        key = publisher.catalog_key(self.current[0])
        self.assertTrue((self.root / '.local-language-publish/stable' / key).is_file())
        self.seed(self.current, legacy=True)
        original = dict(self.store)
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--check')
        self.assertEqual(self.store, original)
        self.assertEqual(self.writes, [])

    def test_immutable_collision_aborts_before_any_catalog_promotion(self):
        key = next(iter(self.current[1]))
        self.store[key] = b'wrong bytes'
        with self.assertRaisesRegex(RuntimeError, 'immutable snapshot collision'):
            self.run_publisher('--publish')
        self.assertEqual(self.writes, [])

    def test_corrupted_upload_is_not_advertised(self):
        self.corrupt_key = next(iter(self.current[1]))
        with self.assertRaisesRegex(RuntimeError, 'read-back mismatch'):
            self.run_publisher('--publish')
        self.assertFalse(any(key.startswith('catalogs/') for key in self.writes))

    def test_interrupted_catalog_promotion_can_be_retried(self):
        self.fail_key = 'catalogs/stable/translation.json'
        with self.assertRaisesRegex(RuntimeError, 'injected publication failure'):
            self.run_publisher('--publish')
        key = publisher.catalog_key(self.current[0])
        self.assertIn(key, self.store)
        self.fail_key = None
        self.run_publisher('--publish')
        self.assertEqual(self.store[key], self.store['catalogs/stable/translation.json'])

    def test_catalog_under_wrong_contract_path_is_rejected(self):
        catalog, _ = bundle(source='d', revision='d')
        self.store[publisher.catalog_key(self.current[0])] = publisher.encode(catalog)
        with self.assertRaisesRegex(RuntimeError, 'wrong identity'):
            self.run_publisher('--publish')
        self.assertEqual(self.writes, [])

    def test_failed_catalog_read_does_not_create_or_replace_catalogs(self):
        with patch.object(publisher, 'remote_get', side_effect=RuntimeError('AccessDenied')):
            with self.assertRaisesRegex(RuntimeError, 'AccessDenied'):
                self.run_publisher('--publish')
        self.assertEqual(self.writes, [])

    def test_invalid_release_stops_before_any_s3_access(self):
        with self.assertRaisesRegex(RuntimeError, 'requires clean'):
            self.run_publisher('--publish', '--release-tag', 'v2.0.0', '--repository', 'uxjulia/CrossInk')
        self.assertEqual(self.reads, [])
        self.assertEqual(self.writes, [])

    def test_prepares_downloads_before_tag_and_github_release_exist(self):
        self.run_publisher('--publish', '--prepare-release', 'v2.0.0', '--source-commit', '1' * 40, clean=True)
        self.assertIn(publisher.catalog_key(self.current[0]), self.store)
        self.assertTrue(self.writes)

    def test_preparation_rejects_wrong_or_dirty_source_before_s3_access(self):
        for source, clean, error in (('2' * 40, True, 'exact full source'),
                                     ('1' * 40, False, 'requires clean')):
            with self.subTest(source=source, clean=clean), self.assertRaisesRegex(RuntimeError, error):
                self.run_publisher('--publish', '--prepare-release', 'v2.0.0',
                                   '--source-commit', source, clean=clean)
            self.assertEqual(self.reads, [])
            self.assertEqual(self.writes, [])


class GitHubExportTest(unittest.TestCase):
    def test_exports_individual_files_and_frozen_catalog_for_each_kind(self):
        bundles = [bundle(), bundle(kind='hyphenation', revision='f')]
        files = github_export.github_files(bundles)
        for catalog, payloads in bundles:
            key = f"catalogs/stable/{catalog['kind']}.json"
            self.assertEqual(files[key], files[f"snapshots/{catalog['revision']}/{key}"])
            for name, data in payloads.items():
                self.assertEqual(files[name], data)
        self.assertFalse(any(name.endswith('.zip') for name in files))

    def test_latest_contract_advances_without_deleting_older_snapshots_or_rc(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            old = github_export.github_files([bundle()])
            rc = github_export.github_files([bundle(channel='rc')])
            github_export.write_files(output, old)
            github_export.write_files(output, rc)
            changed = github_export.github_files([bundle(source='d', revision='d')])
            github_export.write_files(output, changed)
            key = 'catalogs/stable/translation.json'
            self.assertEqual((output / key).read_bytes(), changed[key])
            for name, data in {**old, **rc}.items():
                if name != key:
                    self.assertEqual((output / name).read_bytes(), data)

    def test_collision_does_not_change_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            original = github_export.github_files([bundle()])
            github_export.write_files(output, original)
            conflicting = github_export.github_files([bundle(text=b'conflict')])
            with self.assertRaisesRegex(RuntimeError, 'snapshot collision'):
                github_export.write_files(output, conflicting)
            for name, data in original.items():
                self.assertEqual((output / name).read_bytes(), data)


class ReleaseIdentityTest(unittest.TestCase):
    def identity(self, tag='v2.0.0', channel=None, commit='1' * 40, dirty='', **changes):
        release = {'tag_name': tag, 'draft': False, 'published_at': '2026-10-23T12:00:00Z',
                   'prerelease': tag.startswith('rc-')}
        release.update(changes)
        with patch.object(publisher.subprocess, 'check_output',
                          side_effect=[commit, dirty, json.dumps(release)]) as command:
            result = publisher.release_identity(tag, 'uxjulia/CrossInk', '1' * 40, channel)
        return result, command

    def test_stable_uses_release_version_and_channel(self):
        result, command = self.identity(tag='v2.1.0')
        self.assertEqual(result, ('2.1.0', 'stable'))
        self.assertEqual(command.call_args_list[0].args[0][-1], 'refs/tags/v2.1.0^{commit}')

    def test_rc_uses_exact_packaging_version_and_channel(self):
        tag = 'rc-next-123abcd'
        result, _ = self.identity(tag=tag)
        self.assertEqual(result, (tag, 'rc'))

    def test_drafts_unpublished_and_mismatched_releases_are_rejected(self):
        for changes in ({'draft': True}, {'published_at': None}, {'prerelease': True},
                        {'tag_name': 'v9.0.0'}):
            with self.subTest(changes=changes), self.assertRaisesRegex(RuntimeError, 'must be published'):
                self.identity(**changes)
        with self.assertRaisesRegex(RuntimeError, 'must be published'):
            self.identity(tag='rc-next-123abcd', prerelease=False)

    def test_wrong_source_dirty_source_and_channel_are_rejected(self):
        for options, error in (({'commit': '2' * 40}, 'does not match release tag'),
                               ({'dirty': ' M english.yaml'}, 'requires clean'),
                               ({'channel': 'rc'}, 'does not match requested channel')):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, error):
                self.identity(**options)

    def test_unsupported_tags_and_repository_are_rejected_before_commands(self):
        with patch.object(publisher.subprocess, 'check_output') as command:
            for tag in ('main', 'rc-next', 'v2.0.0/../../main', '--help'):
                with self.subTest(tag=tag), self.assertRaisesRegex(RuntimeError, 'unsupported'):
                    publisher.release_identity(tag, 'uxjulia/CrossInk', '1' * 40)
            with self.assertRaisesRegex(RuntimeError, 'owner/repository'):
                publisher.release_identity('v2.0.0', None, '1' * 40)
            command.assert_not_called()


class PreparedReleaseIdentityTest(unittest.TestCase):
    commit = '123abcd' + '1' * 33

    def identity(self, tag='v2.0.0', source_commit=None, dirty='', tag_commit=None,
                 tag_error=1, channel=None):
        ref = subprocess.CompletedProcess([], tag_error if tag_commit is None else 0,
                                          stdout=tag_commit or '', stderr='lookup failure')
        with patch.object(publisher.subprocess, 'check_output', return_value=dirty) as commands, \
             patch.object(publisher.subprocess, 'run', return_value=ref) as tags:
            result = publisher.prepared_release_identity(tag, source_commit or self.commit,
                                                        self.commit, channel)
        return result, commands, tags

    def test_preparation_does_not_need_a_tag_or_published_release(self):
        result, commands, tags = self.identity()
        self.assertEqual(result, ('2.0.0', 'stable'))
        self.assertIn('status', commands.call_args.args[0])
        self.assertEqual(commands.call_count, 1)
        self.assertEqual(tags.call_args.args[0][-1], 'refs/tags/v2.0.0^{commit}')

    def test_candidate_tag_uses_the_actual_source_hash(self):
        self.assertEqual(self.identity('rc-next-123abcd')[0], ('rc-next-123abcd', 'rc'))
        with self.assertRaisesRegex(RuntimeError, 'candidate tag does not match'):
            self.identity('rc-next-7654321')

    def test_wrong_or_abbreviated_source_is_rejected_before_commands(self):
        with patch.object(publisher.subprocess, 'check_output') as commands, \
             patch.object(publisher.subprocess, 'run') as tags:
            for source in ('main', self.commit[:7], '2' * 40):
                with self.subTest(source=source), self.assertRaisesRegex(RuntimeError, 'exact full source'):
                    publisher.prepared_release_identity('v2.0.0', source, self.commit)
            commands.assert_not_called()
            tags.assert_not_called()

    def test_dirty_source_and_existing_mismatched_tag_are_rejected(self):
        for options, error in (({'dirty': ' M english.yaml'}, 'requires clean'),
                               ({'tag_commit': '2' * 40}, 'does not match existing'),
                               ({'tag_error': 128}, 'could not verify'),
                               ({'channel': 'rc'}, 'requested channel')):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, error):
                self.identity(**options)
        self.assertEqual(self.identity(tag_commit=self.commit)[0], ('2.0.0', 'stable'))

    def test_real_git_source_and_tag_validation(self):
        # Exercise actual git return codes, including a not-yet-created tag.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args], text=True).strip()
            git('init', '-q')
            git('config', 'user.name', 'Release test')
            git('config', 'user.email', 'release-test@example.invalid')
            git('config', 'commit.gpgsign', 'false')
            git('config', 'tag.gpgsign', 'false')
            (root / 'source').write_text('first')
            git('add', 'source')
            git('commit', '-qm', 'first')
            first = git('rev-parse', 'HEAD')
            with patch.object(publisher, 'root', root):
                self.assertEqual(publisher.prepared_release_identity('v2.0.0', first, first), ('2.0.0', 'stable'))
                git('tag', 'v2.0.0')
                (root / 'source').write_text('second')
                with self.assertRaisesRegex(RuntimeError, 'requires clean'):
                    publisher.prepared_release_identity('v2.0.0', first, first)
                git('commit', '-qam', 'second')
                second = git('rev-parse', 'HEAD')
                with self.assertRaisesRegex(RuntimeError, 'does not match existing'):
                    publisher.prepared_release_identity('v2.0.0', second, second)

    def test_both_command_lines_require_preparation_and_source_together(self):
        for script in ('publish_language_assets.py', 'export_github_language_assets.py'):
            for options in (['--prepare-release', 'v2.0.0'], ['--source-commit', self.commit]):
                args = [sys.executable, str(ROOT / 'scripts' / script), *options]
                if script.startswith('export_'):
                    args += ['--output', 'unused']
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn('must be used together', result.stderr)


class ReleaseWorkflowTest(unittest.TestCase):
    def jobs(self, filename):
        text = (ROOT / '.github/workflows' / filename).read_text().split('\njobs:\n', 1)[1]
        parts = re.split(r'^  ([a-z0-9-]+):\n', text, flags=re.M)
        return dict(zip(parts[1::2], parts[2::2]))

    def needs(self, job):
        match = re.search(r'^    needs:([^\n]*)\n((?:      - [^\n]*\n)*)', job, re.M)
        self.assertIsNotNone(match)
        return ([match[1].strip()] if match[1].strip() else
                re.findall(r'^      - ([a-z0-9-]+)$', match[2], re.M))

    def test_stable_and_rc_drafts_require_successful_download_preparation(self):
        for filename, draft_job, build_job in (
                ('release.yml', 'publish-release', 'build-release'),
                ('release_candidate.yml', 'publish-release-candidate', 'build-release-candidate')):
            with self.subTest(workflow=filename):
                jobs = self.jobs(filename)
                draft, gate = jobs[draft_job], jobs['prepare-language-assets']
                self.assertIn('softprops/action-gh-release@', draft)
                self.assertIn('draft: true', draft)
                self.assertIn('prepare-language-assets', self.needs(draft))
                self.assertIn(build_job, self.needs(gate))
                self.assertIn('scripts/prepare_release_downloads.py --tag', gate)
                self.assertIn('SOURCE_COMMIT: ${{ github.sha }}', gate)
                self.assertIn('GH_TOKEN: ${{ secrets.RELEASE_PAT }}', gate)
                self.assertNotIn('continue-on-error:', draft + gate)
                self.assertNotIn('always()', draft + gate)
                # GitHub's default needs semantics must block the draft for
                # failure, cancellation and a skipped preparation workflow.
                for outcome in ('failure', 'cancelled', 'skipped'):
                    results = {job: 'success' for job in self.needs(draft)}
                    results['prepare-language-assets'] = outcome
                    self.assertFalse(all(results[job] == 'success' for job in self.needs(draft)))

    def test_shared_workflow_prepares_both_destinations_without_release_events(self):
        path = ROOT / '.github/workflows/publish-language-assets.yml'
        text = path.read_text()
        self.assertIn('  workflow_dispatch:', text)
        self.assertNotIn('  release:', text)
        self.assertNotIn('github.event.release', text)
        jobs = self.jobs(path.name)
        self.assertEqual(set(jobs), {'verify-source', 'publish-github', 'publish'})
        verification = jobs['verify-source']
        self.assertIn('$GITHUB_REF_TYPE\" == tag', verification)
        self.assertIn('$GITHUB_SHA\" == \"$SOURCE_COMMIT', verification)
        for name in ('publish-github', 'publish'):
            job = jobs[name]
            self.assertEqual(self.needs(job), ['verify-source'])
            self.assertIn('ref: ${{ inputs.source_commit }}', job)
            self.assertIn('--prepare-release "$RELEASE_TAG" --source-commit "$SOURCE_COMMIT"', job)
            self.assertNotIn('continue-on-error:', job)
        rc = self.jobs('release_candidate.yml')
        self.assertIn('rc_tag: ${{ steps.metadata.outputs.rc_tag }}', rc['build-release-candidate'])
        self.assertIn('RELEASE_TAG: ${{ needs.build-release-candidate.outputs.rc_tag }}', rc['prepare-language-assets'])


class ReleaseBundleTest(unittest.TestCase):
    def test_preparation_and_published_retry_have_identical_immutable_bytes(self):
        with patch.object(publisher, 'args', SimpleNamespace(channel='stable', release_tag=None,
                                                            prepare_release='v2.0.0'), create=True):
            prepared = publisher.build_languages('2.0.0', '1' * 40)
        with patch.object(publisher, 'args', SimpleNamespace(channel='stable', release_tag='v2.0.0',
                                                            prepare_release=None), create=True):
            published = publisher.build_languages('2.0.0', '1' * 40)
        self.assertEqual(prepared, published)

    def test_published_translations_match_release_editor_artifacts(self):
        from generate_language_template import generate
        version, commit = 'rc-next-123abcd', '1' * 40
        with patch.object(publisher, 'args', SimpleNamespace(channel='rc', release_tag=version), create=True):
            catalog, files = publisher.build_languages(version, commit)
        expected = generate(version, commit, 'release-build')
        prefix = f"snapshots/{catalog['revision']}/"
        for name, data in expected.items():
            self.assertEqual(files[prefix + 'editor/' + name], data)
        for entry in catalog['entries']:
            filename = 'translations/' + entry['sha256'] + '.yaml'
            self.assertEqual(files[prefix + 'translation/' + entry['sha256'] + '.yaml'], expected[filename])
        self.assertEqual(json.loads(files[prefix + 'snapshot.json'])['status'], 'release-build')
        publisher.validate_companions(catalog, files)

    def test_hyphenation_payloads_match_release_zip_and_ignore_release_label(self):
        from package_hyphenation import artifacts
        expected = artifacts(ROOT)
        with patch.object(publisher, 'args', SimpleNamespace(channel='rc'), create=True):
            rc, rc_files = publisher.build_hyphenation()
        with patch.object(publisher, 'args', SimpleNamespace(channel='stable'), create=True):
            stable, stable_files = publisher.build_hyphenation()
        self.assertEqual(rc['revision'], stable['revision'])
        self.assertEqual(rc_files, stable_files)
        for entry in rc['entries']:
            key = f"snapshots/{rc['revision']}/hyphenation/{entry['sha256']}.cphyph"
            self.assertEqual(rc_files[key], expected[f".crossink/hyphenation/hyph-{entry['code']}.cphyph"])
        publisher.validate_companions(rc, rc_files)


if __name__ == '__main__':
    unittest.main()
