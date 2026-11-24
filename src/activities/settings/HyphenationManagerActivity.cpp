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
  // Allocate labels once per activity. They outlive popup renders but need not
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
  if (languages.size > HYPHENATION_LANGUAGE_COUNT) {
    LOG_ERR("HYPH", "Language list exceeds catalog capacity");
    finish();
    return;
  }
  int builtInIndex = -1;
  for (size_t i = 0; i < languages.size; ++i) {
    const auto& entry = languages.data[i];
    catalog_->installed[i] = HyphenationPackStore::isInstalled(entry.primaryTag);
    if (entry.hyphenator) builtInIndex = static_cast<int>(i);
    if (!std::strcmp(wanted_, entry.primaryTag)) selected_ = static_cast<int>(i);
  }
  catalog_->localCount = 0;
  for (size_t i = 0; i < hyphenation_pack::MAX_PACKS; ++i) {
    char code[3];
    if (!HyphenationPackStore::installedCodeAt(i, code)) break;
    if (!findLanguageEntry(code)) {
      if (!std::strcmp(wanted_, code)) selected_ = static_cast<int>(languages.size + catalog_->localCount);
      std::memcpy(catalog_->local[catalog_->localCount++], code, sizeof(code));
    }
  }
  if (selected_ < 0 || size_t(selected_) >= languages.size + catalog_->localCount) selected_ = 0;
  wanted_[0] = '\0';
  popup_.showBorrowed(
      StrId::STR_HYPHENATION_PACKS,
      OptionLabels(this, languages.size + catalog_->localCount,
                   [](const void* context, size_t index) {
                     return static_cast<const HyphenationManagerActivity*>(context)->labelAt(index);
                   }),
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
const char* HyphenationManagerActivity::codeAt(size_t index) const {
  const auto languages = getLanguageEntries();
  if (index < languages.size) return languages.data[index].primaryTag;
  index -= languages.size;
  return index < catalog_->localCount ? catalog_->local[index] : nullptr;
}
const char* HyphenationManagerActivity::nameAt(size_t index) const {
  const char* code = codeAt(index);
  if (!code) return "";
  const auto* entry = findLanguageEntry(code);
  if (!entry) return code;
  if (entry->displayName) return entry->displayName;
  const char uiCode[] = {static_cast<char>(code[0] - 'a' + 'A'), static_cast<char>(code[1] - 'a' + 'A'), '\0'};
  return I18N.getLanguageName(I18n::languageFromCode(uiCode));
}
const char* HyphenationManagerActivity::labelAt(size_t index) const {
  const char* code = codeAt(index);
  if (!code) return "";
  const auto languages = getLanguageEntries();
  const bool builtIn = index < languages.size && languages.data[index].hyphenator;
  const bool installed = index >= languages.size || catalog_->installed[index];
  const char* status = builtIn     ? tr(STR_HYPHENATION_BUILT_IN)
                       : installed ? tr(STR_INSTALLED)
                                   : tr(STR_HYPHENATION_INSTALL_ACTION);
  // This is a normal borrowed list, not a paired action popup. It measures
  // and draws each label immediately under the activity's render lock.
  std::snprintf(label_, sizeof(label_), "%s (%s): %s", nameAt(index), code, status);
  return label_;
}
void HyphenationManagerActivity::select() {
  const char* code = selected_ < 0 ? nullptr : codeAt(size_t(selected_));
  const auto* entry = findLanguageEntry(code);
  if (!code || (entry && entry->hyphenator)) return;
  if (!HyphenationPackStore::isInstalled(code)) {
    // An SD copy installs offline; otherwise the Wi-Fi screen downloads and installs it.
    if (HyphenationPackStore::hasSource(code))
      apply(false);
    else
      connect();
    return;
  }
  RenderLock lock(*this);
  const char* options[] = {tr(STR_UPDATE), tr(STR_DELETE), tr(STR_CANCEL)};
  const int firstOption = entry ? 0 : 1;  // Unlisted installed SD packs are removable, not downloadable.
  popup_.show(
      nameAt(size_t(selected_)), options + firstOption, 3 - firstOption, 2 - firstOption,
      [this, firstOption](int index) {
        switch (index + firstOption) {
          case 0:
            pending_ = Pending::Connect;
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
  if (selected_ < 0 || size_t(selected_) >= getLanguageEntries().size) return;
  APP_STATE.pendingOverlayResume.clear();
  if (APP_STATE.saveToFile() && SETTINGS.saveToFile()) {
    RenderLock restartLock(*this);
    silentRestartToNetwork(NetworkBootTarget::DOWNLOAD_ASSETS,
                           AssetDownloadActivity::bootPayload(AssetCatalog::Kind::Hyphenation, selected_));
    return;
  }
  LOG_ERR("ASSET", "Cannot save state before network restart");
  pending_ = Pending::SaveError;
}
void HyphenationManagerActivity::apply(bool remove) {
  RenderLock lock(*this);
  const char* code = selected_ < 0 ? nullptr : codeAt(size_t(selected_));
  const auto* entry = findLanguageEntry(code);
  if (!code || (entry && entry->hyphenator)) return;
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
