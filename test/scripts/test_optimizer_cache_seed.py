#!/usr/bin/env python3
"""Compile the production optimizer seeding routine with deterministic file/ZIP I/O."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
text = (ROOT / 'lib/Epub/Epub.cpp').read_text()
start = text.index('bool Epub::seedOptimizerImageCache(')
function = text[start:text.index('\n}', start) + 2]
with tempfile.TemporaryDirectory(prefix='crossink-optimizer-seed-') as tmp:
    tmp = Path(tmp)
    (tmp / 'Seed.inc').write_text(function)
    executable = tmp / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(tmp), '-I' + str(ROOT / 'test/pxc_v2/stubs'), '-I' + str(ROOT),
                    str(ROOT / 'test/pxc_v2/OptimizerCacheSeedTest.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: exact-size PXC seeding bytes, damaged headers, short output, replacement failure, and resize fallback')
