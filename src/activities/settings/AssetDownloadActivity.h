#pragma once

#include <AssetCatalog.h>
#include <AssetCompanionIndex.h>
#include <Memory.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "network/AssetDownloadService.h"

// A network-boot-only controller. It installs the one requested language or
// hyphenation pack end to end (download, verify, activate) and reuses the
// existing flash installers. There is no catalog list or actions menu.
class AssetDownloadActivity final : public Activity {
 public:
  AssetDownloadActivity(GfxRenderer& renderer, MappedInputManager& input, uint32_t payload);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return busy_; }
  bool requiresFreshBackdrop() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  static uint32_t bootPayload(AssetCatalog::Kind kind, uint8_t languageIndex = 0);
  static bool validBootPayload(uint32_t payload);

 private:
#ifdef SIMULATOR
  friend struct AssetDownloadSmokeTest;
  StrId simulatorLastError_ = StrId::_COUNT;
#endif
  enum class Pending { None, Fetch, Download, Overwrite, Activate, Restart, Exit, Install };
  AssetCatalog::Kind kind_;
  uint8_t wantedIndex_;
  AssetCatalog::Selection selection_;
  AssetDownloadService::Source source_;
  OptionPopup popup_;
  Pending pending_ = Pending::None;
  bool busy_ = false;
  bool catalogReady_ = false;
  bool restartRequired_ = false;
  bool cancelled_ = false;
  bool overwriteApproved_ = false;
  char progress_[64] = {};
  // Fixed install stages (catalog, companions, payload); 0 hides the counter.
  uint8_t step_ = 0;
  char stepLabel_[32] = {};
  uint32_t lastProgressAt_ = 0;
  StrId phase_ = StrId::STR_LOADING;
  char url_[AssetDownloadService::MAX_URL] = {};
  AssetDownloadService::Endpoints endpoints_;
  AssetDownloadService::Endpoints catalogEndpoints_;
  StrId title() const;
  bool pollCancel();
  AssetDownloadService::Callbacks callbacks();
  void showBusy(StrId phase = StrId::STR_LOADING);
  OptionPopup::Note busyNote();
  void payloadStarted();
  void progress(size_t received, size_t total);
  void verifying();
  void installSelected();  // discover -> (confirm overwrite) -> download -> activate.
  void showError(StrId error, Pending next = Pending::Exit);
  void confirmOverwrite();
  void fetch();
  void download();
  void activate();
};

static_assert(sizeof(AssetCatalog::Catalog) + sizeof(AssetCatalog::Parser) + sizeof(AssetDownloadActivity) < 16384,
              "Catalog, parser and fixed activity storage exceed C3 catalog RAM ceiling");
static_assert(sizeof(AssetCatalog::Catalog) + sizeof(AssetCompanionIndex::Parser) + sizeof(AssetDownloadActivity) <
                  16384,
              "Catalog, companion parser and fixed activity storage exceed C3 catalog RAM ceiling");
