#!/usr/bin/env python3
"""Pin release source and wait for language downloads before creating a draft."""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from urllib.parse import quote

import publish_language_assets as publisher

WORKFLOW = 'publish-language-assets.yml'
POLL_INTERVAL_SECONDS = 10
VISIBILITY_TIMEOUT_SECONDS = 60
DOWNLOAD_TIMEOUT_SECONDS = 40 * 60


class GitHubApiError(RuntimeError):
    def __init__(self, path, detail):
        super().__init__(f'GitHub API request failed for {path}: {detail}')
        match = re.search(r'\(HTTP (\d{3})\)$', detail)
        self.status = int(match.group(1)) if match else None


def api(repository, path, data=None):
    command = ['gh', 'api', '-H', 'X-GitHub-Api-Version: 2026-03-10', f'repos/{repository}/{path}']
    if data is not None:
        command += ['--method', 'POST', '--input', '-']
    try:
        result = subprocess.run(command, input=None if data is None else json.dumps(data),
                                capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or f'gh api exited with status {exc.returncode}').strip()
        raise GitHubApiError(path, detail) from exc
    return json.loads(result.stdout)


def require_workflow(repository):
    try:
        workflow = api(repository, f'actions/workflows/{WORKFLOW}')
    except RuntimeError as exc:
        raise RuntimeError(
            f'Cannot access {WORKFLOW}. The workflow must exist on the repository default branch '
            'before GitHub can dispatch it on a release tag. Add the workflow there, and ensure '
            f'RELEASE_PAT can access Actions in {repository} (write is required to dispatch). {exc}'
        ) from exc
    if workflow.get('state') != 'active':
        raise RuntimeError(f'{WORKFLOW} must be active before preparing release downloads')


def require_remote_tag(repository, tag, commit, create=False):
    ref = 'refs/tags/' + tag
    matches = api(repository, 'git/matching-refs/tags/' + quote(tag, safe=''))
    if any(item['ref'] == ref for item in matches):
        resolved = api(repository, 'commits/' + quote(tag, safe=''))
        if resolved.get('sha') != commit:
            raise RuntimeError('remote release tag points to another source commit')
    elif create:
        api(repository, 'git/refs', {'ref': ref, 'sha': commit})
    else:
        raise RuntimeError('remote release tag disappeared during preparation')


def wait_for_downloads(repository, run_id, tag, commit):
    started = time.monotonic()
    seen_run = False
    while time.monotonic() - started < DOWNLOAD_TIMEOUT_SECONDS:
        try:
            run = api(repository, f'actions/runs/{run_id}')
        except GitHubApiError as exc:
            # Dispatch can return a run ID before the run endpoint can find it.
            if exc.status != 404 or seen_run or time.monotonic() - started >= VISIBILITY_TIMEOUT_SECONDS:
                raise
            print(f'Run {run_id} is not visible yet (HTTP 404); retrying.', flush=True)
        else:
            seen_run = True
            if (run.get('head_sha') != commit or run.get('head_branch') != tag
                    or run.get('event') != 'workflow_dispatch'):
                raise RuntimeError('language workflow does not match the exact release source')
            if run.get('status') == 'completed':
                if run.get('conclusion') != 'success':
                    raise RuntimeError(f"language workflow {run_id} concluded with {run.get('conclusion')}")
                return
        time.sleep(POLL_INTERVAL_SECONDS)
    raise RuntimeError(f'timed out waiting for language workflow {run_id}')


def prepare(repository, tag, commit):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository or ''):
        raise RuntimeError('a GitHub owner/repository is required')
    current = subprocess.check_output(['git', '-C', str(publisher.root), 'rev-parse', 'HEAD'], text=True).strip()
    publisher.prepared_release_identity(tag, commit, current)
    require_workflow(repository)
    require_remote_tag(repository, tag, commit, create=True)
    # The returned run ID avoids accidentally waiting for an older successful
    # run, or a concurrent dispatch for the same release.
    dispatched = api(repository, f'actions/workflows/{WORKFLOW}/dispatches', {
        'ref': tag,
        'inputs': {'tag': tag, 'source_commit': commit},
    })
    run_id = dispatched.get('workflow_run_id')
    if type(run_id) is not int or run_id <= 0:
        raise RuntimeError('language workflow did not return its run ID')
    url = f'https://github.com/{repository}/actions/runs/{run_id}'
    print('Waiting for release downloads: ' + url, flush=True)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a') as output:
            output.write(f'Language and hyphenation preparation: [{tag}]({url})\n')
    wait_for_downloads(repository, run_id, tag, commit)
    require_remote_tag(repository, tag, commit)
    print('Release downloads are ready; firmware draft creation can proceed.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY'))
    args = parser.parse_args()
    prepare(args.repository, args.tag, args.source_commit)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'Error: {exc}')
