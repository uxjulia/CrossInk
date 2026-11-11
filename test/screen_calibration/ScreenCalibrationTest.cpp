#include <HalScreenCalibration.h>
#include <nvs.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include "../../src/activities/settings/ScreenCalibrationLayout.h"

namespace {
std::vector<uint8_t> saved, pending;
int openError = ESP_OK, readError = ESP_OK, writeError = ESP_OK, commitError = ESP_OK;
int handles = 0, writes = 0;
void reset() {
  saved.clear();
  pending.clear();
  openError = readError = writeError = commitError = ESP_OK;
  handles = writes = 0;
}
}  // namespace

esp_err_t nvs_open(const char* name, int, nvs_handle_t* handle) {
  assert(std::strcmp(name, "screen-cal") == 0);
  if (openError) return openError;
  *handle = 1;
  ++handles;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* buffer, size_t* length) {
  assert(std::strcmp(key, "bounds") == 0);
  if (readError) return readError;
  if (saved.empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (*length < saved.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  *length = saved.size();
  std::memcpy(buffer, saved.data(), saved.size());
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* buffer, size_t length) {
  ++writes;
  if (writeError) return writeError;
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  pending.assign(bytes, bytes + length);
  return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
  if (commitError) return commitError;
  saved = pending;
  return ESP_OK;
}
void nvs_close(nvs_handle_t) {
  --handles;
  pending.clear();
}

int main() {
  for (bool touch : {false, true}) {
    int previousHeight = 0;
    for (int height : {640, 768, 792, 800, 960, 1200}) {
      const auto layout = ScreenCalibrationLayout::fit(height, 24, 68, touch ? 0 : 40, touch);
      assert(layout.rowHeight > previousHeight);
      assert(layout.stepSize <= layout.rowHeight - 8 && layout.stepSize > 0);
      const int actionBottom = layout.panelY + 8 * layout.rowHeight + 6 * layout.ROW_GAP + layout.SECTION_GAP;
      const int legend = touch ? 0 : 3 * (24 + 4) + layout.SECTION_GAP;
      assert(actionBottom + legend <= height - 68);
      assert(height - 68 - actionBottom - legend < 8);
      if (touch && height >= 768) assert(layout.rowHeight >= 56 && layout.stepSize >= 48);
      previousHeight = layout.rowHeight;
    }
  }

  const ScreenInsets defaults = HalScreenCalibration::defaults();
#if FREEINK_DEVICE_EEGO_A4
  assert((defaults.edges == std::array<uint8_t, 4>{{10, 10, 10, 10}}));
  const auto panel = HalScreenCalibration::panelLayout();
  assert(panel.headerOffsetY == 6 && panel.statusBarOffsetY == 6 && panel.tabBarOffsetY == 18);
  assert(panel.statusBarAtCalibratedEdge);
  assert(panel.tabBarWidthPct == 80 && panel.tabBarHeightPct == 80);
  assert((defaults.corners(0) == std::array<int, 4>{{35, 35, 35, 35}}));
  assert((defaults.corners(1) == std::array<int, 4>{{35, 35, 35, 35}}));
  assert((defaults.corners(2) == std::array<int, 4>{{35, 35, 35, 35}}));
  assert((defaults.corners(3) == std::array<int, 4>{{35, 35, 35, 35}}));
  const auto tabs = panel.tabs(24, 672, 504, 72, 752);
  assert(tabs[0] >= 24 && tabs[0] + tabs[2] <= 528);
  assert(tabs[1] + tabs[3] <= 752 && tabs[3] == 57);
#else
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC
  assert((defaults.edges == std::array<uint8_t, 4>{{9, 7, 3, 7}}));
#elif defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  assert((defaults.edges == std::array<uint8_t, 4>{{0, 0, 0, 0}}));
#else
  assert((defaults.edges == std::array<uint8_t, 4>{{9, 3, 3, 3}}));
#endif
  const auto panel = HalScreenCalibration::panelLayout();
  assert((defaults.corners(0) == std::array<int, 4>{{0, 0, 0, 0}}));
  assert(panel.headerOffsetY == 0 && panel.statusBarOffsetY == 0 && panel.tabBarOffsetY == 0);
  assert(!panel.statusBarAtCalibratedEdge);
  assert(panel.tabBarWidthPct == 100 && panel.tabBarHeightPct == 100);
#endif
  ScreenInsets calibrated{{{4, 12, 20, 32}}};
  calibrated.cornerInsets = {{60, 10}};
  assert((calibrated.corners(0) == std::array<int, 4>{{60, 60, 10, 10}}));
  assert((calibrated.corners(1) == std::array<int, 4>{{10, 60, 60, 10}}));
  assert((calibrated.corners(2) == std::array<int, 4>{{10, 10, 60, 60}}));
  assert((calibrated.corners(3) == std::array<int, 4>{{60, 10, 10, 60}}));
  auto cornerDraft = calibrated;
  cornerDraft.adjustCorner(0, 200);
  cornerDraft.adjustCorner(1, -200);
  cornerDraft.adjustCorner(2, 1);
  assert((cornerDraft.cornerInsets == std::array<uint8_t, 2>{{96, 0}}));
  assert(cornerDraft.edges == calibrated.edges);
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    assert(panel.topStatusBarY(cornerDraft, rotation, 12) == panel.topStatusBarY(calibrated, rotation, 12));
    assert(cornerDraft.rotated(rotation).edges[2] == calibrated.rotated(rotation).edges[2]);
  }
  const std::array<std::array<uint8_t, 4>, 4> expected{
      {{{4, 12, 20, 32}}, {{32, 4, 12, 20}}, {{20, 32, 4, 12}}, {{12, 20, 32, 4}}}};
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    assert(calibrated.rotated(rotation).edges == expected[rotation]);
    assert(calibrated.rotated(rotation).rotated(4 - rotation) == calibrated);
  }
  // Default and custom geometry both keep status content inside each physical edge.
  const ScreenInsets maximum{{{32, 32, 32, 32}}};
  for (unsigned rotation = 0; rotation < 4; ++rotation) {
    for (const auto& insets : {defaults, calibrated, maximum, ScreenInsets{{{0, 0, 0, 0}}}}) {
      for (const int legacy : {0, 5, 11, 20}) {
#if FREEINK_DEVICE_EEGO_A4
        // Calibration is the whole edge allowance, without theme/board padding.
        assert(panel.topStatusBarY(insets, rotation, legacy) == insets.rotated(rotation).edges[0]);
#else
        assert(panel.topStatusBarY(insets, rotation, legacy) == insets.topOrigin(rotation, legacy));
#endif
      }
    }
    for (const int legacy : {-5, 0, 5, 10}) {
      assert(defaults.topOrigin(rotation, legacy) ==
             std::max<int>(defaults.rotated(rotation).edges[0],
                           legacy + defaults.rotated(rotation).edges[0] - ScreenInsets{}.rotated(rotation).edges[0]));
      const int top = maximum.rotated(rotation).edges[0];
      const int adjusted = legacy + top - ScreenInsets{}.rotated(rotation).edges[0];
      assert(maximum.topOrigin(rotation, legacy) == (adjusted < top ? top : adjusted));
      assert(maximum.topOrigin(rotation, legacy) >= top);
    }
  }
  ScreenInsets draft = calibrated;
  draft.adjust(0, -100);
  draft.adjust(1, 100);
  draft.adjust(4, 1);
  assert(draft.edges[0] == 0 && draft.edges[1] == 32);
  assert(calibrated.edges[0] == 4 && calibrated.edges[1] == 12);
  assert(draft.valid());
  reset();
  assert(HalScreenCalibration::load() == defaults);
  assert(handles == 0);
  openError = ESP_FAIL;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  openError = ESP_ERR_NVS_NOT_FOUND;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  openError = ESP_OK;
  assert(HalScreenCalibration::save(calibrated));
  assert(HalScreenCalibration::load() == calibrated);
  assert(saved.size() == 10 && saved[2] == 2 && handles == 0);
  const auto original = saved;
  // Upgrade a real v1 record without losing edge calibration.
  saved = {'S', 'C', 1, 4, 12, 20, 32, 0};
  for (size_t i = 0; i < 7; ++i) saved[7] ^= saved[i];
  const auto legacy = HalScreenCalibration::load();
  assert(legacy.edges == calibrated.edges && legacy.cornerInsets == defaults.cornerInsets);
  assert(HalScreenCalibration::save(legacy) && saved.size() == 10 && saved[2] == 2);
  saved = original;
  // Draft edits are not persisted until Save; reset is also only a draft.
  draft = HalScreenCalibration::load();
  draft.adjust(2, 1);
  assert(HalScreenCalibration::load() == calibrated);
  draft = defaults;
  assert(HalScreenCalibration::load() == calibrated);
  for (int failure = 0; failure < 3; ++failure) {
    openError = failure == 0 ? ESP_FAIL : ESP_OK;
    writeError = failure == 1 ? ESP_FAIL : ESP_OK;
    commitError = failure == 2 ? ESP_FAIL : ESP_OK;
    assert(!HalScreenCalibration::save(defaults));
    assert(handles == 0 && saved == original);
  }
  openError = writeError = commitError = ESP_OK;
  readError = ESP_FAIL;
  assert(HalScreenCalibration::load() == defaults && handles == 0);
  readError = ESP_OK;
  for (size_t byte = 0; byte < original.size(); ++byte) {
    saved = original;
    saved[byte] ^= 1;
    assert(HalScreenCalibration::load() == defaults && handles == 0);
  }
  saved = original;
  saved[2] = 99;
  saved[9] ^= original[2] ^ 99;  // Future version with otherwise valid encoding.
  assert(HalScreenCalibration::load() == defaults);
  saved = original;
  saved[3] = 33;
  saved[9] ^= original[3] ^ 33;  // Valid checksum, invalid range.
  assert(HalScreenCalibration::load() == defaults);
  saved = original;
  saved.push_back(0);
  assert(HalScreenCalibration::load() == defaults);
  saved.resize(3);
  assert(HalScreenCalibration::load() == defaults);
  saved = original;
  saved[8] = 97;
  saved[9] ^= original[8] ^ 97;
  assert(HalScreenCalibration::load() == defaults);
  auto invalidCorner = calibrated;
  invalidCorner.cornerInsets[0] = 97;
  assert(!HalScreenCalibration::save(invalidCorner));
  ScreenInsets invalid;
  invalid.edges[0] = 255;
  const int beforeWrites = writes;
  assert(!HalScreenCalibration::save(invalid) && writes == beforeWrites);
  assert(HalScreenCalibration::save(defaults));
  assert(HalScreenCalibration::load() == defaults && handles == 0);
}
