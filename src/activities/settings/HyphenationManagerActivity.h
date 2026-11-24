#pragma once

#include <Memory.h>

#include "HyphenationPackStore.h"
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
  const char* simulatorLabel(size_t index) const { return labelAt(index); }
  bool simulatorOptionDisabled(int index) const { return popup_.simulatorOptionDisabled(index); }
  const char* simulatorOptionLabel(int index) const { return popup_.simulatorOptionLabel(index); }
#endif

 private:
#ifdef SIMULATOR
  friend struct AssetDownloadSmokeTest;
#endif
  static constexpr size_t LOCAL_CAPACITY = 16;
  struct Catalog {
    bool installed[HYPHENATION_LANGUAGE_COUNT] = {};
    HyphenationPackStore::LocalPack local[LOCAL_CAPACITY] = {};
    size_t localCount = 0;
  };
  std::unique_ptr<Catalog> catalog_;
  OptionPopup popup_;
  mutable char label_[128] = {};
  char wanted_[3] = {};
  int selected_ = 0;
  enum class Pending { None, List, Select, Install, Remove, Connect, SaveError };
  Pending pending_ = Pending::None;
  void showLanguages();
  const char* labelAt(size_t index) const;
  const char* codeAt(size_t index) const;
  const char* nameAt(size_t index) const;
  void select();
  void apply(bool remove);
  void connect();
};
