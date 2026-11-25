#!/usr/bin/env python3
"""Compile the production bounded EPUB cache reader against controlled file seams."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/Epub/Epub/BookMetadataCache.cpp').read_text()
constants = re.findall(r'^constexpr [^\n]+ (?:BOOK_CACHE_(?:MAGIC|VERSION)|bookBinFile\[\])[^\n]*$', source, re.M)
assert len(constants) == 3
start = source.index('namespace {\n// A book.bin metadata field')
end = source.index('void BookMetadataCache::cacheSpineCumulativeSizes', start)
with tempfile.TemporaryDirectory(prefix='crossink-metadata-bounds-') as tmp:
    build = Path(tmp)
    (build / 'BoundedMetadataLoad.inc').write_text('\n'.join(constants) + '\n' + source[start:end])
    executable = build / 'test'
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    sanitizers = ['-fsanitize=address,undefined'] if os.environ.get('CROSSINK_TEST_SANITIZERS') == '1' else []
    subprocess.run([*compiler, '-std=c++20', '-Wall', '-Wextra', *sanitizers,
                    '-I' + str(build), str(ROOT / 'test/book_metadata_bounds/MetadataBoundsTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: valid v10, empty fields, bounds, truncation, invalid LUT, cache reopen')
