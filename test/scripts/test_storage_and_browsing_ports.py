#!/usr/bin/env python3
"""Run production storage and browsing routines against deterministic host I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'

parts = [function('src/activities/reader/StatsBackup.cpp', signature) for signature in (
    'bool isStatsBackupFileName(', 'bool copyString(', 'int pruneBackups(')]
parts += [function('src/activities/library/LibraryActivity.cpp', 'void LibraryActivity::loadGridPageCovers(')]
parts += [function('src/activities/home/RecentBookProgress.cpp', signature) for signature in (
    'float loadEpubSizeProgressPercentFromCachePath(', 'float loadEpubProgressPercent(')]
with tempfile.TemporaryDirectory(prefix='crossink-storage-browsing-') as tmp:
    tmp = Path(tmp)
    (tmp / 'Production.inc').write_text('\n'.join(parts))
    executable = tmp / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(tmp),
                    str(ROOT / 'test/storage_and_browsing/StorageAndBrowsingTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: cached progress reads, statistics backup retention, and batched cover redraws')
