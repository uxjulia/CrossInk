#pragma once
#include <cstdio>
#define LOG_DBG(tag, fmt, ...) std::printf(tag " " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) std::printf(tag " " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(tag, fmt, ...) std::printf(tag " " fmt "\n", ##__VA_ARGS__)
