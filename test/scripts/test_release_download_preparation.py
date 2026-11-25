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
        self.run_responses = []
        self.elapsed = 0
        self.failed_create = False
        self.workflow_state = 'active'
        self.workflow_error = None

    def api(self, repository, path, data=None):
        self.assertEqual(repository, self.repository)
        self.calls.append((path, data))
        if path == 'actions/workflows/' + preparation.WORKFLOW:
            if self.workflow_error:
                raise RuntimeError(self.workflow_error)
            return {'state': self.workflow_state}
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
        if self.run_responses:
            response = self.run_responses.pop(0)
            if isinstance(response, Exception):
                raise response
            return response
        return self.result

    def sleep(self, seconds):
        self.elapsed += seconds

    def prepare(self):
        with patch.object(preparation, 'api', side_effect=self.api), \
             patch.object(preparation.publisher, 'prepared_release_identity') as identity, \
             patch.object(preparation.subprocess, 'check_output', return_value=self.commit), \
             patch.object(preparation.time, 'monotonic', side_effect=lambda: self.elapsed), \
             patch.object(preparation.time, 'sleep', side_effect=self.sleep), \
             patch.dict('os.environ', {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
            preparation.prepare(self.repository, self.tag, self.commit)
            identity.assert_called_once_with(self.tag, self.commit, self.commit)

    def test_waits_for_the_returned_run_before_accepting_success(self):
        self.run_responses = [preparation.GitHubApiError('actions/runs/71', 'gh: Not Found (HTTP 404)'),
                              {**self.result, 'status': 'queued', 'conclusion': None},
                              {**self.result, 'status': 'in_progress', 'conclusion': None}]
        self.prepare()
        paths = [path for path, _ in self.calls]
        self.assertLess(paths.index('git/refs'), paths.index('actions/workflows/publish-language-assets.yml/dispatches'))
        self.assertEqual(paths.count('actions/workflows/publish-language-assets.yml/dispatches'), 1)
        self.assertEqual(paths.count('actions/runs/71'), 4)
        self.assertEqual(paths[-1], 'commits/' + self.tag)

    def test_persistent_404_expires_at_the_visibility_deadline(self):
        error = preparation.GitHubApiError('actions/runs/71', 'gh: Not Found (HTTP 404)')
        self.run_responses = [error] * 10
        with self.assertRaisesRegex(RuntimeError, 'HTTP 404'):
            self.prepare()
        self.assertEqual(self.elapsed, preparation.VISIBILITY_TIMEOUT_SECONDS)

    def test_forbidden_lookup_fails_immediately(self):
        self.run_responses = [preparation.GitHubApiError('actions/runs/71', 'gh: Forbidden (HTTP 403)')]
        with self.assertRaisesRegex(RuntimeError, 'HTTP 403'):
            self.prepare()
        self.assertEqual(self.elapsed, 0)

    def test_404_after_run_is_visible_fails_immediately(self):
        self.run_responses = [{**self.result, 'status': 'in_progress', 'conclusion': None},
                              preparation.GitHubApiError('actions/runs/71', 'gh: Not Found (HTTP 404)')]
        with self.assertRaisesRegex(RuntimeError, 'HTTP 404'):
            self.prepare()
        self.assertEqual(self.elapsed, preparation.POLL_INTERVAL_SECONDS)

    def test_unfinished_downloads_time_out(self):
        self.result.update(status='in_progress', conclusion=None)
        with self.assertRaisesRegex(RuntimeError, 'timed out waiting for language workflow 71'):
            self.prepare()
        self.assertEqual(self.elapsed, preparation.DOWNLOAD_TIMEOUT_SECONDS)

    def test_retry_uses_existing_correct_tag_without_replacing_it(self):
        self.remote_commit = self.commit
        self.prepare()
        self.assertNotIn('git/refs', [path for path, _ in self.calls])

    def test_unavailable_workflow_explains_default_branch_before_creating_tag(self):
        self.workflow_error = 'gh: Not Found (HTTP 404)'
        with self.assertRaisesRegex(RuntimeError, 'default branch.*HTTP 404'):
            self.prepare()
        self.assertIsNone(self.remote_commit)
        self.assertEqual(len(self.calls), 1)

    def test_disabled_workflow_blocks_tag_creation_and_dispatch(self):
        self.workflow_state = 'disabled_manually'
        with self.assertRaisesRegex(RuntimeError, 'must be active'):
            self.prepare()
        self.assertIsNone(self.remote_commit)
        self.assertEqual(len(self.calls), 1)

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
        for conclusion in ('failure', 'cancelled', 'skipped'):
            self.result['conclusion'] = conclusion
            with self.subTest(conclusion=conclusion), self.assertRaisesRegex(RuntimeError, f'concluded with {conclusion}'):
                self.prepare()
        self.assertEqual(self.remote_commit, self.commit)
        self.assertEqual(self.calls[-1][0], 'actions/runs/71')

    def test_missing_run_id_is_not_replaced_by_an_older_success(self):
        for value in ({}, {'workflow_run_id': None}, {'workflow_run_id': True}):
            self.dispatched = value
            with self.subTest(response=value), self.assertRaisesRegex(RuntimeError, 'run ID'):
                self.prepare()

    def test_run_from_wrong_source_is_rejected(self):
        original = self.result.copy()
        for field, value in (('head_sha', '2' * 40), ('head_branch', 'main'), ('event', 'push')):
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

    def test_api_failure_keeps_github_error_details(self):
        path = 'actions/workflows/test/dispatches'
        for stderr, stdout, expected in (
                ('gh: Not Found (HTTP 404)', '', 'HTTP 404'),
                ('gh: Resource not accessible (HTTP 403)', '', 'HTTP 403'),
                ('', '{"message":"Not Found"}', 'Not Found'),
                ('', '', 'status 1')):
            failure = subprocess.CalledProcessError(1, ['gh', 'api'], output=stdout, stderr=stderr)
            with self.subTest(stderr=stderr, stdout=stdout), \
                 patch.object(preparation.subprocess, 'run', side_effect=failure), \
                 self.assertRaisesRegex(RuntimeError, path + '.*' + expected):
                preparation.api(self.repository, path, {'ref': self.tag})


if __name__ == '__main__':
    unittest.main()
