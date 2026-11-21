#pragma once

#include <Memory.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class HyphenationManagerActivity final : public Activity {
 public:
  HyphenationManagerActivity(GfxRenderer& renderer, MappedInputManager& input, const char* wanted = nullptr);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool requiresFreshBackdrop() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
#ifdef SIMULATOR
  const char* simulatorLabel(size_t index) const { return catalog_->rows[index].label; }
  bool simulatorOptionDisabled(int index) const { return popup_.simulatorOptionDisabled(index); }
  const char* simulatorOptionLabel(int index) const { return popup_.simulatorOptionLabel(index); }
#endif

 private:
#ifdef SIMULATOR
  friend struct AssetDownloadSmokeTest;
#endif
  static constexpr size_t KNOWN_CAPACITY = 10;
  static constexpr size_t LOCAL_CAPACITY = 16;
  struct Row {
    char label[128] = {};
    char code[3] = {};
    int onlineIndex = -1;
  };
  struct Catalog {
    Row rows[KNOWN_CAPACITY + LOCAL_CAPACITY];
    size_t count = 0;
  };
  std::unique_ptr<Catalog> catalog_;
  OptionPopup popup_;
  char wanted_[3] = {};
  int selected_ = 0;
  enum class Pending { None, List, Select, Install, Remove, Connect, SaveError };
  Pending pending_ = Pending::None;
  void showLanguages();
  const Row* selectedRow() const;
  void select();
  void apply(bool remove);
  void connect();
};
