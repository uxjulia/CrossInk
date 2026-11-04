#!/usr/bin/env python3
"""Exercise release gating without dispatching workflows or creating remote tags."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_release_downloads as preparation


class ReleasePreparationTest(unittest.TestCase):
    repository = 'uxjulia/CrossInk'
    commit = '123abcd' + '1' * 33
    tag = 'v2.0.0'

    def setUp(self):
        self.remote_commit = None
        self.calls = []
        self.result = {'status': 'completed', 'conclusion': 'success', 'head_sha': self.commit,
                       'head_branch': self.tag, 'event': 'workflow_dispatch'}
        self.dispatched = {'workflow_run_id': 71}
        self.failed_watch = False
        self.failed_create = False

    def api(self, repository, path, data=None):
        self.assertEqual(repository, self.repository)
        self.calls.append((path, data))
        if path.startswith('git/matching-refs/'):
            return [{'ref': 'refs/tags/' + self.tag}] if self.remote_commit else []
        if path.startswith('commits/'):
            return {'sha': self.remote_commit}
        if path == 'git/refs':
            if self.failed_create:
                raise RuntimeError('tag creation denied')
            self.assertEqual(data, {'ref': 'refs/tags/' + self.tag, 'sha': self.commit})
            self.remote_commit = self.commit
            return {}
        if path.endswith('/dispatches'):
            self.assertEqual(data, {'ref': self.tag, 'inputs': {'tag': self.tag, 'source_commit': self.commit}})
            return self.dispatched
        self.assertEqual(path, 'actions/runs/71')
        return self.result

    def watch(self, command, **options):
        self.assertEqual(command, ['gh', 'run', 'watch', '71', '--repo', self.repository,
                                   '--exit-status', '--interval', '10'])
        self.assertTrue(options['check'])
        if self.failed_watch:
            raise subprocess.CalledProcessError(1, command)

    def prepare(self):
        with patch.object(preparation, 'api', side_effect=self.api), \
             patch.object(preparation.publisher, 'prepared_release_identity') as identity, \
             patch.object(preparation.subprocess, 'check_output', return_value=self.commit), \
             patch.object(preparation.subprocess, 'run', side_effect=self.watch) as watch, \
             patch.dict('os.environ', {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
            preparation.prepare(self.repository, self.tag, self.commit)
            identity.assert_called_once_with(self.tag, self.commit, self.commit)
        return watch

    def test_waits_for_the_returned_run_before_accepting_success(self):
        watch = self.prepare()
        watch.assert_called_once()
        paths = [path for path, _ in self.calls]
        self.assertLess(paths.index('git/refs'), paths.index('actions/workflows/publish-language-assets.yml/dispatches'))
        self.assertEqual(paths[-1], 'commits/' + self.tag)

    def test_retry_uses_existing_correct_tag_without_replacing_it(self):
        self.remote_commit = self.commit
        self.prepare()
        self.assertNotIn('git/refs', [path for path, _ in self.calls])

    def test_wrong_existing_tag_blocks_dispatch(self):
        self.remote_commit = '2' * 40
        with self.assertRaisesRegex(RuntimeError, 'another source commit'):
            self.prepare()
        self.assertFalse(any(path.endswith('/dispatches') for path, _ in self.calls))

    def test_tag_creation_failure_blocks_dispatch(self):
        self.failed_create = True
        with self.assertRaisesRegex(RuntimeError, 'creation denied'):
            self.prepare()
        self.assertFalse(any(path.endswith('/dispatches') for path, _ in self.calls))

    def test_failed_child_run_blocks_completion_and_retains_tag_for_retry(self):
        self.failed_watch = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(self.remote_commit, self.commit)
        self.assertNotIn('actions/runs/71', [path for path, _ in self.calls])

    def test_missing_run_id_is_not_replaced_by_an_older_success(self):
        for value in ({}, {'workflow_run_id': None}, {'workflow_run_id': True}):
            self.dispatched = value
            with self.subTest(response=value), self.assertRaisesRegex(RuntimeError, 'run ID'):
                self.prepare()

    def test_wrong_or_unsuccessful_run_is_rejected_even_if_watch_exits_zero(self):
        original = self.result.copy()
        for field, value in (('head_sha', '2' * 40), ('head_branch', 'main'), ('event', 'push'),
                             ('status', 'in_progress'), ('conclusion', 'failure'),
                             ('conclusion', 'cancelled'), ('conclusion', 'skipped')):
            self.result = {**original, field: value}
            with self.subTest(field=field, value=value), self.assertRaisesRegex(RuntimeError, 'exact release source'):
                self.prepare()

    def test_invalid_source_blocks_all_remote_access(self):
        with patch.object(preparation, 'api') as api, \
             patch.object(preparation.subprocess, 'check_output', return_value=self.commit), \
             patch.object(preparation.publisher, 'prepared_release_identity', side_effect=RuntimeError('dirty source')):
            with self.assertRaisesRegex(RuntimeError, 'dirty source'):
                preparation.prepare(self.repository, self.tag, self.commit)
        api.assert_not_called()

    def test_dispatch_uses_documented_run_id_api_version(self):
        response = subprocess.CompletedProcess([], 0, json.dumps(self.dispatched), '')
        with patch.object(preparation.subprocess, 'run', return_value=response) as command:
            self.assertEqual(preparation.api(self.repository, 'actions/workflows/test/dispatches',
                                              {'ref': self.tag}), self.dispatched)
        self.assertIn('X-GitHub-Api-Version: 2026-03-10', command.call_args.args[0])
        self.assertEqual(json.loads(command.call_args.kwargs['input']), {'ref': self.tag})


if __name__ == '__main__':
    unittest.main()
