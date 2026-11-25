#include "HyphenationManagerActivity.h"

#include <Epub/hyphenation/LanguageRegistry.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "AssetDownloadActivity.h"
#include "CrossPointState.h"
#include "HyphenationPackStore.h"
#include "SilentRestart.h"

HyphenationManagerActivity::HyphenationManagerActivity(GfxRenderer& renderer, MappedInputManager& input,
                                                       const char* wanted)
    : Activity("HyphenationManager", renderer, input) {
  if (wanted && std::strlen(wanted) == 2) std::memcpy(wanted_, wanted, sizeof(wanted_));
}
void HyphenationManagerActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  // Allocate once per activity: labels outlive popup renders, but need not
  // consume permanent RAM or the small C3 main-task stack.
  catalog_ = makeUniqueNoThrow<Catalog>();
  if (!catalog_) {
    LOG_ERR("HYPH", "OOM: %u byte language list", unsigned(sizeof(Catalog)));
    finish();
    return;
  }
  showLanguages();
}
void HyphenationManagerActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  popup_.clear();
  catalog_.reset();
  Activity::onExit();
}
void HyphenationManagerActivity::showLanguages() {
  RenderLock lock(*this);
  const auto languages = getLanguageEntries();
  if (languages.size > KNOWN_CAPACITY) {
    LOG_ERR("HYPH", "Language list exceeds catalog capacity");
    finish();
    return;
  }
  catalog_->count = 0;
  int builtInIndex = -1;
  const auto appendRow = [&](const char* code, const char* status, int onlineIndex) {
    const size_t i = catalog_->count++;
    auto& row = catalog_->rows[i];
    std::memcpy(row.code, code, sizeof(row.code));
    row.onlineIndex = onlineIndex;
    const char uiCode[] = {static_cast<char>(code[0] - 'a' + 'A'), static_cast<char>(code[1] - 'a' + 'A'), '\0'};
    const auto language = I18n::languageFromCode(uiCode);
    // Unknown UI language codes fall back to English; use the pack code instead.
    if (language == Language::EN && std::strcmp(code, "en") != 0) {
      std::snprintf(row.label, sizeof(row.label), "%s: %s", code, status);
    } else {
      std::snprintf(row.label, sizeof(row.label), "%s (%s): %s", I18N.getLanguageName(language), code, status);
    }
    if (!std::strcmp(wanted_, code)) selected_ = static_cast<int>(i);
  };
  for (size_t i = 0; i < languages.size; ++i) {
    const auto& entry = languages.data[i];
    if (entry.hyphenator) builtInIndex = static_cast<int>(i);
    appendRow(entry.primaryTag,
              entry.hyphenator                                      ? tr(STR_HYPHENATION_BUILT_IN)
              : HyphenationPackStore::isInstalled(entry.primaryTag) ? tr(STR_INSTALLED)
                                                                    : tr(STR_HYPHENATION_INSTALL_ACTION),
              entry.hyphenator ? -1 : static_cast<int>(i));
  }
  // Catalog languages can also be present in the SD/cache union. Leave room
  // for them so they do not count against the local-only row limit.
  HyphenationPackStore::LocalPack local[KNOWN_CAPACITY + LOCAL_CAPACITY];
  const size_t count = HyphenationPackStore::listLocalPacks(local, KNOWN_CAPACITY + LOCAL_CAPACITY);
  for (size_t i = 0; i < count && catalog_->count < languages.size + LOCAL_CAPACITY; ++i) {
    if (findLanguageEntry(local[i].code)) continue;
    appendRow(local[i].code, local[i].installed ? tr(STR_INSTALLED) : tr(STR_HYPHENATION_INSTALL_ACTION), -1);
  }
  if (selected_ < 0 || size_t(selected_) >= catalog_->count) selected_ = 0;
  wanted_[0] = '\0';
  popup_.showBorrowed(
      StrId::STR_HYPHENATION_PACKS,
      OptionLabels(
          catalog_.get(), catalog_->count,
          [](const void* context, size_t index) { return static_cast<const Catalog*>(context)->rows[index].label; }),
      selected_,
      [this](int index) {
        selected_ = index;
        pending_ = Pending::Select;
      },
      OptionPopup::Note(tr(STR_HYPHENATION_APPLY_HINT), ""));
  popup_.setInertOptionIndex(builtInIndex);
  popup_.setCancelCallback([this] { finish(); });
  requestUpdate();
}
const HyphenationManagerActivity::Row* HyphenationManagerActivity::selectedRow() const {
  if (selected_ < 0 || size_t(selected_) >= catalog_->count) return nullptr;
  const auto& row = catalog_->rows[selected_];
  const auto* entry = findLanguageEntry(row.code);
  return entry && entry->hyphenator ? nullptr : &row;
}
void HyphenationManagerActivity::select() {
  const auto* row = selectedRow();
  if (!row) return;
  const bool localOnly = row->onlineIndex < 0;
  const bool hasSource = HyphenationPackStore::hasSource(row->code);
  if (!HyphenationPackStore::isInstalled(row->code)) {
    if (hasSource)
      apply(false);
    else if (localOnly)
      showLanguages();
    else
      connect();
    return;
  }
  RenderLock lock(*this);
  const bool canUpdate = !localOnly || hasSource;
  const char* options[] = {tr(STR_UPDATE), tr(STR_DELETE), tr(STR_CANCEL)};
  const int firstOption = canUpdate ? 0 : 1;
  popup_.show(
      row->label, options + firstOption, 3 - firstOption, 2 - firstOption,
      [this, firstOption, localOnly](int index) {
        switch (index + firstOption) {
          case 0:
            pending_ = localOnly ? Pending::Install : Pending::Connect;
            break;
          case 1:
            pending_ = Pending::Remove;
            break;
          default:
            pending_ = Pending::List;
            break;
        }
      },
      OptionPopup::Note(tr(STR_HYPHENATION_APPLY_HINT), ""));
  popup_.setCancelCallback([this] { pending_ = Pending::List; });
  requestUpdate();
}
void HyphenationManagerActivity::connect() {
  const auto* row = selectedRow();
  if (!row || row->onlineIndex < 0) return;
  APP_STATE.pendingOverlayResume.clear();
  if (APP_STATE.saveToFile() && SETTINGS.saveToFile()) {
    RenderLock restartLock(*this);
    silentRestartToNetwork(NetworkBootTarget::DOWNLOAD_ASSETS,
                           AssetDownloadActivity::bootPayload(AssetCatalog::Kind::Hyphenation, row->onlineIndex));
    return;
  }
  LOG_ERR("ASSET", "Cannot save state before network restart");
  pending_ = Pending::SaveError;
}
void HyphenationManagerActivity::apply(bool remove) {
  RenderLock lock(*this);
  const auto* row = selectedRow();
  if (!row) return;
  const char* code = row->code;
  popup_.clear();
  GUI.drawPopup(renderer, tr(STR_LOADING), true);
  // Do not commit a pack if SD state cannot be saved for the following restart.
  auto result = hyphenation_pack::Result::Io;
  APP_STATE.pendingOverlayResume.clear();
  if (APP_STATE.saveToFile())
    result = remove ? HyphenationPackStore::remove(code) : HyphenationPackStore::install(code);
  if (result == hyphenation_pack::Result::Ok) {
    // The running reader and its suspended layouts still point to the old bank.
    // Activation happens only on boot; no mapped pointers are invalidated here.
    silentRestart();
    return;
  }
  if (result == hyphenation_pack::Result::Unchanged) {
    pending_ = Pending::List;
    return;
  }
  LOG_ERR("HYPH", "Pack operation: %s", hyphenation_pack::resultName(result));
  const StrId options[] = {StrId::STR_OK};
  popup_.show(
      StrId::STR_HYPHENATION_PACKS, options, 1, 0, [this](int) { pending_ = Pending::List; },
      OptionPopup::Note(tr(STR_UPDATE_FAILED), tr(STR_HYPHENATION_FAILED)));
  popup_.setCancelCallback([this] { pending_ = Pending::List; });
  requestUpdate();
}
void HyphenationManagerActivity::loop() {
  // Do not replace a popup from inside its own selection callback.
  const auto pending = pending_;
  pending_ = Pending::None;
  switch (pending) {
    case Pending::List:
      showLanguages();
      return;
    case Pending::Select:
      select();
      return;
    case Pending::SaveError: {
      RenderLock lock(*this);
      const StrId options[] = {StrId::STR_OK};
      popup_.show(
          StrId::STR_HYPHENATION_PACKS, options, 1, 0, [this](int) { pending_ = Pending::List; },
          OptionPopup::Note(tr(STR_ASSET_WIFI_DOWNLOAD), tr(STR_LANGUAGE_SAVE_FAILED)));
      popup_.setCancelCallback([this] { pending_ = Pending::List; });
      requestUpdate();
      return;
    }
    case Pending::Connect:
      connect();
      return;
    case Pending::Install:
      apply(false);
      return;
    case Pending::Remove:
      apply(true);
      return;
    case Pending::None:
      break;
  }
  popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}
void HyphenationManagerActivity::render(RenderLock&&) {
  if (!popup_.isActive()) return;
  renderer.clearScreen();
  popup_.processRender(renderer, mappedInput);
}
