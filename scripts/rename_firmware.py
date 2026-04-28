"""
PlatformIO post-build script: copy firmware.bin to convenient artifact names
in the same build directory.

Default outputs:
  .pio/build/tiny/firmware-tiny.bin
  .pio/build/xlarge/firmware-xlarge.bin
  .pio/build/no_emoji/firmware-no_emoji.bin

Release-candidate outputs when CROSSPOINT_RC_ARTIFACTS=1:
  .pio/build/tiny/firmware-tiny-<branch>-<hash>-RC.bin
"""

import os
import re
import shutil
import subprocess
import sys


def _copy_artifact(src, dst):
    shutil.copy(src, dst)
    print(f'Firmware copied to: {dst}')


def _get_project_option(env, name):
    try:
        value = env.GetProjectOption(name)
    except Exception:
        return None
    if isinstance(value, str):
        value = value.strip()
    return value or None


def _get_git_value(project_dir, *args, fallback):
    try:
        return subprocess.check_output(
            ['git', *args],
            text=True,
            stderr=subprocess.PIPE,
            cwd=project_dir,
        ).strip()
    except Exception:
        return fallback


def _get_git_branch(project_dir):
    branch = _get_git_value(
        project_dir,
        'rev-parse',
        '--abbrev-ref',
        'HEAD',
        fallback='unknown',
    )
    if branch == 'HEAD':
        return _get_git_value(
            project_dir,
            'rev-parse',
            '--short',
            'HEAD',
            fallback='unknown',
        )
    return branch


def _sanitize_branch(branch):
    if branch.startswith('release/'):
        branch = branch[len('release/'):]
    branch = branch.strip()
    branch = re.sub(r'[^A-Za-z0-9._-]+', '-', branch)
    branch = branch.strip('-.')
    return branch or 'unknown'


def _get_rc_artifact_name(project_dir, env):
    env_name = _get_project_option(env, 'custom_rc_variant') or env['PIOENV']
    branch = (
        _get_project_option(env, 'custom_rc_branch')
        or os.environ.get('CROSSPOINT_RC_BRANCH')
        or _get_git_branch(project_dir)
    )
    short_hash = (
        _get_project_option(env, 'custom_rc_hash')
        or os.environ.get('CROSSPOINT_RC_HASH')
        or _get_git_value(
        project_dir,
        'rev-parse',
        '--short',
        'HEAD',
        fallback='00000',
    )
    )
    branch = _sanitize_branch(branch)
    short_hash = re.sub(r'[^A-Za-z0-9]+', '', short_hash)[:12] or '00000'
    return f'firmware-{env_name}-{branch}-{short_hash}-RC.bin'


def _is_rc_artifact_build(env):
    flag = _get_project_option(env, 'custom_rc_artifacts') or os.environ.get('CROSSPOINT_RC_ARTIFACTS')
    return str(flag).strip().lower() in {'1', 'true', 'yes', 'on'}


def rename_firmware(source, target, env):
    env_name = env['PIOENV']
    project_dir = env['PROJECT_DIR']
    src = str(target[0])
    build_dir = os.path.dirname(src)

    default_dst = os.path.join(build_dir, f'firmware-{env_name}.bin')
    _copy_artifact(src, default_dst)

    if _is_rc_artifact_build(env):
        rc_dst = os.path.join(build_dir, _get_rc_artifact_name(project_dir, env))
        _copy_artifact(src, rc_dst)


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        rename_firmware,
    )
except NameError:
    print('rename_firmware.py: must be run via PlatformIO', file=sys.stderr)
