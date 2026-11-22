#pragma once

#include <string>

namespace SleepBookSync {
// Runs once after the reader has closed and the sleep screen has been drawn.
// The caller supplies Wi-Fi setup so progress can be mapped before Wi-Fi uses heap.
void run(const std::string& path, bool progressSafe, bool (*ensureConnected)());
}  // namespace SleepBookSync
