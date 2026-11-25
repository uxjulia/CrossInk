#pragma once
#include <cstdio>
#define LOG_ERR(tag, ...)                         \
  do {                                            \
    if (false) std::fprintf(stderr, __VA_ARGS__); \
  } while (0)
