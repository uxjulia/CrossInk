#pragma once

#if defined(CROSSINK_LANGUAGE_BENCHMARK)
#include <Logging.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#if defined(SIMULATOR)
#include <chrono>
#else
#include <esp_timer.h>
#endif

namespace language_benchmark {
inline uint64_t now() {
#if defined(SIMULATOR)
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
#else
  return esp_timer_get_time();
#endif
}
inline std::atomic<TaskHandle_t> owner{nullptr};
inline uint64_t started = 0, displayTime = 0;
inline char activity[64] = {};
inline void beginFrame(const char* name) {
  std::snprintf(activity, sizeof(activity), "%s", name);
  displayTime = 0;
  started = now();
  owner.store(xTaskGetCurrentTaskHandle());
}
struct DisplayScope {
  const uint64_t start = owner.load() == xTaskGetCurrentTaskHandle() ? now() : 0;
  ~DisplayScope() {
    if (start) displayTime += now() - start;
  }
};
inline void endFrame() {
  const uint64_t elapsed = now() - started;
  owner.store(nullptr);
  LOG_INF("LANG-TIMING", "activity=%s cpu_us=%llu display_call_us=%llu total_us=%llu", activity,
          static_cast<unsigned long long>(elapsed >= displayTime ? elapsed - displayTime : 0),
          static_cast<unsigned long long>(displayTime), static_cast<unsigned long long>(elapsed));
}
}  // namespace language_benchmark
#endif
