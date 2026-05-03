"""
PlatformIO pre-build script: inject git info into version defines.

  default:       1.1.0-dev+<branch>
  test & debug:          1.2.6-<branch>+<5-char-hash>
  gh_release_rc: 1.1.0-rc+<5-char-hash>   (hash from $CROSSPOINT_RC_HASH in
                                             CI, or from git locally)

All other environments set CROSSPOINT_VERSION directly in platformio.ini.
"""

import configparser
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def get_git_short_hash(project_dir, length=5):
    try:
        return subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()[:length]
    except Exception as e:
        warn(f'Could not read git hash: {e}; hash will be "00000"')
        return '00000'


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return sanitize_version_component(branch)


def sanitize_version_component(value):
    value = value.strip()
    value = re.sub(r'[^A-Za-z0-9._-]+', '-', value)
    value = re.sub(r'-{2,}', '-', value)
    value = value.strip('-.')
    return value or 'unknown'


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def _read_ini(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    config = configparser.ConfigParser()
    if os.path.isfile(ini_path):
        config.read(ini_path)
    else:
        warn(f'platformio.ini not found at {ini_path}')
    return config


def get_base_version(project_dir):
    config = _read_ini(project_dir)
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def get_crossink_version(project_dir):
    config = _read_ini(project_dir)
    if not config.has_option('crosspoint', 'crossink_version'):
        warn('No [crosspoint] crossink_version in platformio.ini; falling back to version')
        return get_base_version(project_dir)
    return config.get('crosspoint', 'crossink_version')


def inject_version(env):
    project_dir = env['PROJECT_DIR']
    pioenv = env['PIOENV']

    if pioenv == 'default':
        base_version = get_base_version(project_dir)
        branch = get_git_branch(project_dir)
        version_string = f'{base_version}-dev+{branch}'
        env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
        print(f'CrossInk build version: {version_string}')

    elif pioenv == 'debug':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        cp_version = get_base_version(project_dir)
        ci_version = get_crossink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        env.Append(CPPDEFINES=[
            ('CROSSPOINT_VERSION', f'\\"{cp_version}{suffix}\\"'),
            ('CROSSINK_VERSION', f'\\"{ci_version}{suffix}\\"'),
        ])
        print(f'CrossInk test build version: {ci_version}{suffix}')

    elif pioenv == 'test':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        cp_version = get_base_version(project_dir)
        ci_version = get_crossink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        env.Append(CPPDEFINES=[
            ('CROSSPOINT_VERSION', f'\\"{cp_version}{suffix}\\"'),
            ('CROSSINK_VERSION', f'\\"{ci_version}{suffix}\\"'),
        ])
        print(f'CrossInk test build version: {ci_version}{suffix}')

    elif pioenv == 'gh_release_rc':
        # CI passes CROSSPOINT_RC_HASH as an env var; locally we derive it from git.
        short_hash = os.environ.get('CROSSPOINT_RC_HASH') or get_git_short_hash(project_dir)
        cp_version = get_base_version(project_dir)
        ci_version = get_crossink_version(project_dir)
        rc_suffix = f'-rc+{short_hash}'
        env.Append(CPPDEFINES=[
            ('CROSSPOINT_VERSION', f'\\"{cp_version}{rc_suffix}\\"'),
            ('CROSSINK_VERSION', f'\\"{ci_version}{rc_suffix}\\"'),
        ])
        print(f'CrossInk RC build version: {cp_version}{rc_suffix}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    if '__file__' in globals():
        _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    else:
        _project_dir = os.getcwd()
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
else:
    inject_version(env)  # noqa: F821  # type: ignore[name-defined]
