#!/usr/bin/env python3
"""Pin release source and wait for language downloads before creating a draft."""
import argparse
import json
import os
import re
import subprocess
import sys
from urllib.parse import quote

import publish_language_assets as publisher

WORKFLOW = 'publish-language-assets.yml'


def api(repository, path, data=None):
    command = ['gh', 'api', '-H', 'X-GitHub-Api-Version: 2026-03-10', f'repos/{repository}/{path}']
    if data is not None:
        command += ['--method', 'POST', '--input', '-']
    result = subprocess.run(command, input=None if data is None else json.dumps(data),
                            capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


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


def prepare(repository, tag, commit):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository or ''):
        raise RuntimeError('a GitHub owner/repository is required')
    current = subprocess.check_output(['git', '-C', str(publisher.root), 'rev-parse', 'HEAD'], text=True).strip()
    publisher.prepared_release_identity(tag, commit, current)
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
    subprocess.run(['gh', 'run', 'watch', str(run_id), '--repo', repository,
                    '--exit-status', '--interval', '10'], check=True)
    result = api(repository, f'actions/runs/{run_id}')
    if (result.get('status') != 'completed' or result.get('conclusion') != 'success'
            or result.get('head_sha') != commit or result.get('head_branch') != tag
            or result.get('event') != 'workflow_dispatch'):
        raise RuntimeError('language workflow did not succeed for the exact release source')
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
