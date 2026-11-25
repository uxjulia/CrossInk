#pragma once
#include <cstdio>
#define LOG_INF(tag, format, ...) std::fprintf(stderr, "[%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_ERR(tag, format, ...) LOG_INF(tag, format, ##__VA_ARGS__)
#define LOG_DBG(tag, format, ...) LOG_INF(tag, format, ##__VA_ARGS__)
