#pragma once

#include <Memory.h>

#include "Epub/hyphenation/LanguageRegistry.h"
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
  const char* simulatorLabel(size_t index) const { return catalog_->labels[index]; }
  bool simulatorOptionDisabled(int index) const { return popup_.simulatorOptionDisabled(index); }
  const char* simulatorOptionLabel(int index) const { return popup_.simulatorOptionLabel(index); }
#endif

 private:
#ifdef SIMULATOR
  friend struct AssetDownloadSmokeTest;
#endif
  struct Catalog {
    char labels[HYPHENATION_LANGUAGE_COUNT][128] = {};
  };
  std::unique_ptr<Catalog> catalog_;
  OptionPopup popup_;
  char wanted_[3] = {};
  int selected_ = 0;
  enum class Pending { None, List, Select, Remove, Connect, SaveError };
  Pending pending_ = Pending::None;
  void showLanguages();
  void select();
  void apply(bool remove);
  void connect();
};
