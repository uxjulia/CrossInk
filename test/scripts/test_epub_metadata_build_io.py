#!/usr/bin/env python3
"""Fault-inject the production reader used while building EPUB metadata caches."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/Epub/Epub/BookMetadataCache.cpp').read_text()
start = source.index('template <typename F, typename T>\nbool readPodChecked')
end = source.index('}  // namespace', start)
with tempfile.TemporaryDirectory(prefix='crossink-metadata-build-io-') as tmp:
    build = Path(tmp)
    accessors = source.index('BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(')
    (build / 'CheckedMetadataReads.inc').write_text(source[start:end] + source[accessors:])
    executable = build / 'test'
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    sanitizers = ['-fsanitize=address,undefined'] if os.environ.get('CROSSINK_TEST_SANITIZERS') == '1' else []
    subprocess.run([*compiler, '-std=c++20', '-Wall', '-Wextra', *sanitizers,
                    '-I' + str(build), str(ROOT / 'test/epub_metadata_build_io/MetadataBuildIoTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

print('PASS: metadata entry failures and live-cache fallback, removal, and short-read paths')
