#pragma once

#include <cstddef>
#include <cstdint>

namespace FsHelpers {

int naturalCompare(const char* s1, const char* s2);
size_t naturalSortKey(const char* name, uint8_t* out, size_t cap);

}  // namespace FsHelpers
