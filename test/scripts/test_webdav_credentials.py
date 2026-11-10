#!/usr/bin/env python3
"""Compile the complete production WebDAV handler against deterministic host I/O.

Only platform includes are replaced. Path normalization is also production code;
no handler, policy, transfer or rollback routine is copied into the test double.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def without_includes(path):
    return '\n'.join(line for line in (ROOT / path).read_text().splitlines()
                     if not line.startswith('#include ') and line != '#pragma once')

source = (ROOT / 'lib/FsHelpers/FsHelpers.cpp').read_text()
start = source.index('std::string normalisePath(')
normalise = source[start:source.index('\n}', start) + 2]
with tempfile.TemporaryDirectory(prefix='crossink-webdav-') as folder:
    folder = Path(folder)
    (folder / 'Production.inc').write_text(
        without_includes('src/network/ProtectedPaths.h') + '\n' +
        'namespace FsHelpers {\n' + normalise + '\n}\n' +
        without_includes('src/network/WebDAVHandler.h') + '\n' +
        without_includes('src/network/WebDAVHandler.cpp'))
    executable = folder / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(folder),
                    str(ROOT / 'test/webdav_credentials/WebDAVCredentialsTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
