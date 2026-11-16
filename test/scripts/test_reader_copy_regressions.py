#!/usr/bin/env python3
"""Execute production COPY, link resolution and reader restore code with host doubles."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'


reader = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
start = reader.index('  bool progressLoaded = false;', reader.index('void EpubReaderActivity::onEnter()'))
end = reader.index('\n  if (resumePageBuildAfterRestart)', start)
parts = [function('src/network/WebDAVHandler.cpp', 'void WebDAVHandler::handleCopy('),
         function('lib/Epub/Epub.cpp', 'int Epub::resolveHrefToSpineIndex('),
         'void Reader::restore() {\n' + reader[start:end] + '\n}\n']
with tempfile.TemporaryDirectory(prefix='crossink-reader-copy-') as directory:
    tmp = Path(directory)
    (tmp / 'Production.inc').write_text('\n'.join(parts))
    binary = tmp / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(tmp),
                    str(ROOT / 'test/reader_copy_regressions/ReaderCopyTest.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: COPY I/O failures, saved spine-zero positions, and exact EPUB link precedence')
