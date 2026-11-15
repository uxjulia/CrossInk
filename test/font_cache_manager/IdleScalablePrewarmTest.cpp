#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "FontCacheManager.h"
#include "FontDecompressor.h"

namespace {
struct ColdFont {
  EpdGlyph glyph;
  int rasters = 0;
  bool failed = false;
  bool slow = false;
  bool* signalDuringMetrics = nullptr;
  std::atomic<bool>* entered = nullptr;
  static const EpdGlyph* metrics(void* ctx, uint32_t cp) {
    auto& f = *static_cast<ColdFont*>(ctx);
    f.glyph.dataOffset = cp;
    if (f.signalDuringMetrics) *f.signalDuringMetrics = true;
    return &f.glyph;
  }
  static const uint8_t* bitmap(void* ctx, const EpdGlyph*) {
    auto& f = *static_cast<ColdFont*>(ctx);
    ++f.rasters;
    if (f.entered) f.entered->store(true);
    if (f.slow) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    static const uint8_t pixel = 0xFF;
    return f.failed ? nullptr : &pixel;
  }
};
struct Fixture : testing::Test {
  ColdFont cold;
  EpdFontData data{nullptr, &cold, ColdFont::bitmap, ColdFont::metrics};
  std::map<int, EpdFontFamily> fonts{{1, EpdFontFamily(&data, nullptr, nullptr, nullptr)}};
  std::map<int, SdCardFont*> sd;
  FontCacheManager manager{fonts, sd};
  FontCacheManager::IdleScalableJob job;
  FontDecompressor decompressor;
  void scan(const char* text) {
    auto scope = manager.createPrewarmScope(FontCacheManager::PreparationPolicy::IdleScalable);
    manager.recordText(text, 1, EpdFontFamily::REGULAR);
    scope.endScanForIdle(job);
  }
};
TEST_F(Fixture, SlicesResumeAndNeverClearVisibleCaches) {
  manager.setFontDecompressor(&decompressor);
  scan("ABCDE");
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr, 2), FontCacheManager::IdleResult::Yielded);
  EXPECT_EQ(cold.rasters, 2);
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr, 2), FontCacheManager::IdleResult::Yielded);
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr, 2), FontCacheManager::IdleResult::Complete);
  EXPECT_EQ(cold.rasters, 5);
  EXPECT_EQ(decompressor.clearCacheCallCount, 0);
}
TEST_F(Fixture, InputArrivingDuringColdRasterStopsBeforeNextGlyph) {
  scan("ABCDE");
  cold.slow = true;
  std::atomic<bool> entered{false}, input{false};
  cold.entered = &entered;
  std::thread buttonOrTouch([&] {
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    input.store(true);
  });
  const auto check = [](void* ctx) { return static_cast<std::atomic<bool>*>(ctx)->load(); };
  const auto result = manager.prewarmIdleSlice(job, check, &input);
  buttonOrTouch.join();
  EXPECT_EQ(result, FontCacheManager::IdleResult::Yielded);
  EXPECT_EQ(cold.rasters, 1);
  EXPECT_EQ(job.cursor, 1);
  input.store(false);
  cold.slow = false;
  EXPECT_EQ(manager.prewarmIdleSlice(job, check, &input), FontCacheManager::IdleResult::Complete);
  EXPECT_EQ(cold.rasters, 5);
}
TEST_F(Fixture, BudgetExpiringDuringMetricsStillCompletesOneGlyph) {
  scan("ABC");
  bool expired = false;
  cold.signalDuringMetrics = &expired;
  const auto budget = [](void* ctx) { return *static_cast<bool*>(ctx); };
  EXPECT_EQ(manager.prewarmIdleSlice(job, budget, &expired), FontCacheManager::IdleResult::Yielded);
  EXPECT_EQ(cold.rasters, 1);
  EXPECT_EQ(job.cursor, 1);
}
TEST_F(Fixture, InputDuringMetricsCancelsBeforeRasterWithoutAdvancing) {
  scan("ABC");
  bool input = false;
  cold.signalDuringMetrics = &input;
  const auto pending = [](void* ctx) { return *static_cast<bool*>(ctx); };
  EXPECT_EQ(manager.prewarmIdleSlice(job, pending, &input, 16, pending), FontCacheManager::IdleResult::Yielded);
  EXPECT_EQ(cold.rasters, 0);
  EXPECT_EQ(job.cursor, 0);
}
TEST_F(Fixture, AbandonedScanDoesNotRasterizeInDestructor) {
  {
    auto scope = manager.createPrewarmScope(FontCacheManager::PreparationPolicy::IdleScalable);
    manager.recordText("ABC", 1, EpdFontFamily::REGULAR);
  }
  EXPECT_FALSE(manager.isScanning());
  EXPECT_EQ(cold.rasters, 0);
}
TEST_F(Fixture, FailedRasterDoesNotAdvanceAndResetDropsOldWork) {
  scan("ABC");
  cold.failed = true;
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr), FontCacheManager::IdleResult::Failed);
  EXPECT_EQ(job.cursor, 0);
  job.reset();
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr), FontCacheManager::IdleResult::Complete);
}
TEST_F(Fixture, ReplacedFontRegistrationIsResolvedFresh) {
  scan("ABC");
  fonts.clear();
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr), FontCacheManager::IdleResult::Complete);
  EXPECT_EQ(cold.rasters, 0);
}
}  // namespace
