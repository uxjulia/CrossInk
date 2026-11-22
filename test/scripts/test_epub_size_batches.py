#!/usr/bin/env python3
"""Compile the actual metadata batch loop with fault-injectable storage seams."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/Epub/Epub/BookMetadataCache.cpp').read_text()
start = source.index('  if (spineCount >= LARGE_SPINE_THRESHOLD) {', source.index('bool BookMetadataCache::buildBookBin'))
end = source.index('  if (tryExactSizeBatch) {', start)
constant = '\n'.join(re.findall(r'constexpr size_t (?:MAX|MIN)_SIZE_LOOKUP_TARGETS = \d+;', source))
with tempfile.TemporaryDirectory(prefix='crossink-size-batches-') as tmp:
    root = Path(tmp)
    (root / 'SizeLookup.inc').write_text(constant + '\n' + source[start:end])
    (root / 'Arena.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
class Arena {
 public:
  unsigned failAllocation = 0;
  unsigned failFromAllocation = 0;
  std::vector<size_t> allocations;
  std::vector<void*> buffers;
  void* alloc(size_t bytes, size_t) {
    allocations.push_back(bytes);
    if (allocations.size() == failAllocation ||
        (failFromAllocation != 0 && allocations.size() >= failFromAllocation)) return nullptr;
    void* buffer = std::malloc(bytes);
    buffers.push_back(buffer);
    return buffer;
  }
  ~Arena() { for (void* buffer : buffers) std::free(buffer); }
};
''')
    executable = root / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-I' + str(root), '-I' + str(ROOT / 'lib/Memory'),
                    str(ROOT / 'test/epub_size_batches/BatchLookupTest.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    const_start = source.index('constexpr uint16_t MIN_EXACT_SIZE_BATCH_SPINE')
    const_end = source.index('// Entry (de)serializers', const_start)
    (root / 'SmallExactBatchConstants.inc').write_text(source[const_start:const_end])
    small_end = source.index('  uint32_t cumSize = 0;', end)
    (root / 'SmallExactBatch.inc').write_text(source[end:small_end])
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-I' + str(root), '-I' + str(ROOT / 'lib/Memory'),
                    str(ROOT / 'test/epub_size_batches/SmallExactBatchTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    zip_source = (ROOT / 'lib/ZipFile/ZipFile.cpp').read_text()
    strict_start = zip_source.index('int ZipFile::fillEntryIdentities(')
    strict_end = zip_source.index('uint8_t* ZipFile::readFileToMemory', strict_start)
    (root / 'ProductionStrictLookup.inc').write_text(zip_source[strict_start:strict_end])
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-I' + str(root), str(ROOT / 'test/zip_entry_identities/StrictLookupTest.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

print('PASS: batch boundaries through 65,535 entries, duplicates, missing entries, allocation failure cleanup; exact matching with forced hash collisions, zero sizes, duplicate and suspect ZIP directories')
