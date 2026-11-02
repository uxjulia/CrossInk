#pragma once

#include <cstdint>

enum class FontLoadError : uint8_t { None, Memory, Read, Unsupported, TooLarge, InvalidFamily, NoFont };
