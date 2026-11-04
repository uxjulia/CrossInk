#ifdef SIMULATOR
#include "AssetDownloadSmokeTest.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HyphenationPackStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/settings/AssetDownloadActivity.h"
#include "activities/settings/HyphenationManagerActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "network/AssetDownloadService.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

// Host-only functional integration. HTTP responses come from a disposable mock
// root; neither .invalid hosts nor simulator TLS qualify a production endpoint.
struct AssetDownloadSmokeTest {
  using Kind = AssetCatalog::Kind;
  using Result = AssetDownloadService::Result;
  using Status = AssetDownloadService::Status;
  using Button = MappedInputManager::Button;
  unsigned phase = 0, inputPhase = 0, settle = 0, offlinePhase = 0, managerPhase = 0, updatePhase = 0,
           settingsPhase = 0;
  int navLeft = 0;
  bool seen = false;
  int touchX = 0, touchY = 0;
  bool touching = false;
  int initialStage = -1;
  Button button = Button::Confirm;
  AssetDownloadService::Source source;
  std::string editedHash;
  std::string activeFlashBefore;
  uint64_t activeGenerationBefore = 0;
  uint32_t patternIdentityBefore = 0;

  static void require(bool condition, const char* message) {
    if (condition) return;
    LOG_ERR("SMOKE", "Asset downloads: %s", message);
    std::_Exit(2);
  }
  static const char* originalUi() { return std::getenv("CROSSINK_ASSET_SMOKE_UI"); }
  static int stage() {
    const char* value = std::getenv("CROSSINK_ASSET_SMOKE_STAGE");
    return value ? std::atoi(value) : 0;
  }
  static void capture(const char* name) {
    const char* output = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
    if (!output) return;
    activityManager.requestUpdateAndWait();
    RenderLock lock;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    require(image, "cannot save capture");
    const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
    std::fprintf(image, "P5\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    require(std::fclose(image) == 0, "capture close failed");
  }
  static void checkProgress() {
    require(APP_STATE.openEpubPath == std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), "book context changed");
    std::ifstream input("fs_/.crosspoint/asset-progress-sentinel", std::ios::binary);
    std::string value;
    std::getline(input, value);
    require(value == "book progress retained", "progress sentinel changed");
    EpubReaderUtils::Progress progress;
    const auto progressPath = Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint") + "/progress.bin";
    require(EpubReaderUtils::readProgressFile("SMOKE", progressPath, progress) && progress.spineIndex == 1 &&
                progress.pageNumber == 2 && progress.pageCount == 10 && progress.hasVisibleTextOffset &&
                progress.visibleTextOffset == 1234,
            "EPUB reading position changed");
  }
  static std::string flashBytes() {
    const char* path = std::getenv("CROSSINK_LANGUAGE_FLASH");
    require(path && *path, "isolated language flash path missing");
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read language flash image");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  static void unchanged(bool hyphenation) {
    require(originalUi() && !std::strcmp(I18N.getCode(), originalUi()), "download changed active UI language");
    const char* generation = std::getenv("CROSSINK_ASSET_SMOKE_UI_GENERATION");
    require(generation && I18N.getGeneration() == std::strtoull(generation, nullptr, 10),
            "download changed active UI generation");
    const char* savedGeneration = std::getenv("CROSSINK_ASSET_SMOKE_SETTINGS_GENERATION");
    require(savedGeneration && SETTINGS.languageCacheGeneration == std::strtoull(savedGeneration, nullptr, 10),
            "download changed saved UI generation");
    require(HyphenationPackStore::isInstalled("de") == !hyphenation, "download changed active hyphenation");
    checkProgress();
  }
  void tap(Button value) {
    button = value;
    inputPhase = 1;
  }
  void tapPopup(OptionPopup& popup, int index) {
    if (!mappedInputManager.hasTouchHardware()) {
      tap(Button::Confirm);
      return;
    }
    activityManager.requestUpdateAndWait();
    const auto hit = popup.simulatorOptionRect(index);
    require(hit.width > 0 && hit.height > 0, "touch option bounds missing");
    touchX = hit.x + hit.width / 2;
    touchY = hit.y + hit.height / 2;
    touching = true;
    inputPhase = 1;
  }
  void tapOption(AssetDownloadActivity& activity, int index) { tapPopup(activity.popup_, index); }
  static void captureMatrix(const char* name) {
    const auto scale = SETTINGS.uiScale;
    const auto orientation = renderer.getOrientation();
    for (uint8_t value : {CrossPointSettings::UI_SCALE_SMALL, CrossPointSettings::UI_SCALE_LARGE}) {
      for (auto rotation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
        {
          RenderLock lock;
          SETTINGS.uiScale = value;
          renderer.setOrientation(rotation);
        }
        const auto file =
            std::string(name) + "-scale-" + std::to_string(value) + "-rotation-" + std::to_string(rotation);
        capture(file.c_str());
      }
    }
    {
      RenderLock lock;
      SETTINGS.uiScale = scale;
      renderer.setOrientation(orientation);
    }
    activityManager.requestUpdateAndWait();
  }
  bool inputTick() {
    if (inputPhase == 1) {
#if CROSSINK_APP_CAP_TOUCH
      if (touching)
        mappedInputManager.simulatorInjectTouchDown(touchX, touchY);
      else
#endif
        mappedInputManager.simulatorInjectPress(button);
      inputPhase = 2;
      return true;
    }
    if (inputPhase == 2) {
#if CROSSINK_APP_CAP_TOUCH
      if (touching)
        mappedInputManager.simulatorInjectTouchRelease(touchX, touchY);
      else
#endif
        mappedInputManager.simulatorInjectRelease(button);
      touching = false;
      inputPhase = 0;
      settle = 4;
      return true;
    }
    if (settle) {
      --settle;
      return true;
    }
    return false;
  }
  static bool ready(const AssetDownloadActivity& activity) {
    return !activity.busy_ && activity.pending_ == AssetDownloadActivity::Pending::None && activity.popup_.isActive();
  }
  static uint8_t germanIndex() {
    const auto languages = getLanguageEntries();
    for (size_t i = 0; i < languages.size; ++i)
      if (!std::strcmp(languages.data[i].primaryTag, "de")) return static_cast<uint8_t>(i);
    require(false, "German missing from language registry");
    return 0;
  }
  static void startNetwork(Kind kind, const char* nextStage) {
    setenv("CROSSINK_ASSET_SMOKE_STAGE", nextStage, 1);
    RenderLock lock;
    silentRestartToNetwork(
        NetworkBootTarget::DOWNLOAD_ASSETS,
        AssetDownloadActivity::bootPayload(kind, kind == Kind::Hyphenation ? germanIndex() : germanTranslationIndex()));
  }
  static std::string localHyphenationSource() {
    std::ifstream input("fs_/.crossink/hyphenation/hyph-de.cphyph", std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  static void success() {
    LOG_INF(
        "SMOKE",
        "Simulator smoke test passed: asset downloads, real SHA/format validation, source status, overwrite Cancel, "
        "collision/cancellation/corruption preservation, explicit activation reboots to Home (mock HTTP only)");
    std::_Exit(0);
  }
  Result discover(AssetDownloadActivity& activity) {
    return AssetDownloadService::discover(activity.kind_, activity.catalog_->entries[activity.selected_], source);
  }
  Result download(AssetDownloadActivity& activity, bool approved = false,
                  const AssetDownloadService::Callbacks& callbacks = {}) {
    return AssetDownloadService::download(*activity.catalog_, activity.catalog_->entries[activity.selected_],
                                          activity.endpoints_, approved, callbacks);
  }
  static const char* kindName(Kind kind) { return kind == Kind::Hyphenation ? "hyphenation" : "translation"; }
  static std::filesystem::path companionRoot(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry) {
    return std::filesystem::path("fs_/.crosspoint/asset-sources") / entry.sha256 / catalog.revision;
  }
  static std::filesystem::path mockSnapshot(const AssetCatalog::Catalog& catalog) {
    return std::filesystem::path(std::getenv("CROSSINK_SIM_HTTP_MOCK_ROOT")) / "snapshots" / catalog.revision;
  }
  static std::filesystem::path expectation(Kind kind, const AssetCatalog::Entry& entry) {
    return std::filesystem::path("asset-expected") / (std::string(kindName(kind)) + "-" + entry.code + ".txt");
  }
  static std::string firstCompanion(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry) {
    std::ifstream list(expectation(catalog.kind, entry));
    std::string relative;
    require(bool(std::getline(list, relative)) && relative.rfind("source/", 0) == 0, "companion fixture list missing");
    return relative;
  }
  static bool sameFile(const std::filesystem::path& left, const std::filesystem::path& right) {
    // Host-only test comparison; firmware verification streams through its own
    // bounded SHA buffer. Compare every byte, including opaque source archives.
    std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
    if (!a || !b) return false;
    std::array<char, 4096> x{}, y{};
    do {
      a.read(x.data(), x.size());
      b.read(y.data(), y.size());
      if (a.gcount() != b.gcount() || std::memcmp(x.data(), y.data(), a.gcount())) return false;
    } while (a.gcount());
    return a.eof() && b.eof();
  }
  static void delivered(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry) {
    const auto root = companionRoot(catalog, entry);
    require(std::filesystem::is_regular_file(root / "complete.txt"),
            "payload reported complete without companion marker");
    std::ifstream list(expectation(catalog.kind, entry));
    require(bool(list), "companion expectation list missing");
    std::string relative;
    size_t count = 0, largest = 0, archives = 0;
    while (std::getline(list, relative)) {
      require(sameFile(root / relative, mockSnapshot(catalog) / relative), "delivered companion/index bytes differ");
      const auto bytes = std::filesystem::file_size(root / relative);
      if (bytes > largest) largest = bytes;
      if (std::filesystem::path(relative).extension() == ".zip") ++archives;
      ++count;
    }
    require(count >= 3, "empty delivered companion set");
    LOG_INF("SMOKE", "Companion closure %s/%s verified: %u files/indexes, largest %u bytes, %u opaque archives",
            kindName(catalog.kind), entry.code, static_cast<unsigned>(count), static_cast<unsigned>(largest),
            static_cast<unsigned>(archives));
  }
  void companionChecks(AssetDownloadActivity& activity, bool initiallyMissing) {
    const auto& catalog = *activity.catalog_;
    const auto& entry = catalog.entries[activity.selected_];
    const auto root = companionRoot(catalog, entry);
    const auto relative = firstCompanion(catalog, entry);
    const auto remote = mockSnapshot(catalog) / relative;
    const auto held = remote.string() + ".test-held";
    const auto originalHash = initiallyMissing ? std::string() : std::string(source.sha256);
    std::filesystem::rename(remote, held);
    const auto missing = download(activity);
    std::filesystem::rename(held, remote);
    require(missing != Result::Ok, "missing companion reported payload complete");
    require(discover(activity) == Result::Ok &&
                (initiallyMissing ? source.status == Status::Missing : source.sha256 == originalHash),
            "missing companion published or changed payload");
    if (initiallyMissing) {
      require(!std::filesystem::exists(root / "complete.txt"), "incomplete closure published marker");
      return;
    }
    delivered(catalog, entry);  // Failed replacement retains the previous complete set.
    std::fstream corrupt(remote, std::ios::binary | std::ios::in | std::ios::out);
    char original = 0;
    require(bool(corrupt.get(original)), "cannot read companion corruption fixture");
    corrupt.seekp(0);
    corrupt.put(original ^ 1);
    corrupt.flush();
    const auto invalid = download(activity);
    corrupt.seekp(0);
    corrupt.put(original);
    corrupt.close();
    require(invalid != Result::Ok, "same-size corrupt companion accepted");
    require(discover(activity) == Result::Ok && source.sha256 == originalHash, "corrupt companion changed payload");
    delivered(catalog, entry);
    require(std::filesystem::remove(root / relative), "cannot remove local companion for repair");
    require(discover(activity) == Result::Ok && source.status == Status::Current,
            "missing companion changed payload identity");
    require(download(activity) == Result::Ok, "Current payload could not repair missing companions");
    delivered(catalog, entry);
    require(discover(activity) == Result::Ok && source.sha256 == originalHash,
            "companion repair changed payload bytes");
    unchanged(activity.kind_ == Kind::Hyphenation);
  }
  void transactionChecks(AssetDownloadActivity& activity) {
    auto& entry = activity.catalog_->entries[activity.selected_];
    require(discover(activity) == Result::Ok && source.status == Status::Current, "download was not current");
    auto previous = entry;
    std::strcpy(previous.previous, entry.sha256);
    std::memset(previous.sha256, 'a', 64);
    require(AssetDownloadService::discover(activity.kind_, previous, source) == Result::Ok &&
                source.status == Status::Previous,
            "previous hash not classified");
    const std::string local = std::string("fs_") + source.path;
    {
      std::ofstream edited(local, std::ios::binary | std::ios::app);
      edited << "\n# local edit\n";
      require(bool(edited), "cannot seed edited source");
    }
    require(discover(activity) == Result::Ok && source.status == Status::Different, "manual bytes not classified");
    editedHash = source.sha256;
    require(download(activity) == Result::OverwriteRequired, "unknown source silently overwritten");
    unchanged(activity.kind_ == Kind::Hyphenation);
  }
  void preservationChecks(AssetDownloadActivity& activity) {
    require(discover(activity) == Result::Ok && source.status == Status::Different && source.sha256 == editedHash,
            "default overwrite Confirm did not cancel");
    require(!activity.overwriteApproved_, "cancel approved replacement");
    require(download(activity, true) == Result::Ok, "approved replacement failed");
    require(discover(activity) == Result::Ok && source.status == Status::Current, "replacement not current");
    const std::string local = std::string("fs_") + source.path;
    const auto duplicate = std::filesystem::path(local).parent_path() /
                           (activity.kind_ == Kind::Hyphenation ? "duplicate.cphyph" : "duplicate.yaml");
    std::filesystem::copy_file(local, duplicate);
    require(discover(activity) == Result::Collision && download(activity, true) == Result::Collision,
            "duplicate source accepted");
    require(std::filesystem::remove(duplicate), "cannot remove duplicate test source");
    struct Cancel {
      bool value = false;
    } cancel;
    AssetDownloadService::Callbacks callbacks;
    callbacks.context = &cancel;
    callbacks.cancelled = [](void* context) { return static_cast<Cancel*>(context)->value; };
    callbacks.progress = [](void* context, size_t done, size_t) {
      if (done) static_cast<Cancel*>(context)->value = true;
    };
    require(download(activity, false, callbacks) == Result::Cancelled, "transfer cancellation failed");
    cancel.value = false;
    callbacks.progress = nullptr;
    callbacks.verifying = [](void* context) { static_cast<Cancel*>(context)->value = true; };
    require(download(activity, false, callbacks) == Result::Cancelled, "verification cancellation failed");
    auto& entry = activity.catalog_->entries[activity.selected_];
    const auto payload = mockSnapshot(*activity.catalog_) / kindName(activity.kind_) /
                         (std::string(entry.sha256) + (activity.kind_ == Kind::Hyphenation ? ".cphyph" : ".yaml"));
    std::fstream mock(payload, std::ios::binary | std::ios::in | std::ios::out);
    require(bool(mock), "mock payload absent");
    char original;
    mock.seekg(-1, std::ios::end);
    mock.get(original);
    mock.seekp(-1, std::ios::end);
    mock.put(original ^ 1);
    mock.flush();
    require(download(activity) == Result::Invalid, "same-size corrupt payload accepted");
    mock.seekp(-1, std::ios::end);
    mock.put(original);
    mock.close();
    require(discover(activity) == Result::Ok && source.status == Status::Current, "failed transfer changed source");
    require(!Storage.exists((std::string(source.path) + ".old").c_str()), "successful replacement kept backup");
    companionChecks(activity, false);
    unchanged(activity.kind_ == Kind::Hyphenation);
  }
  // Every other supplied catalog asset still downloads through real streaming
  // transport, SHA, format/capacity validation and SD publication.
  void downloadCatalog(AssetDownloadActivity* activity) {
    for (size_t i = 0; i < activity->catalog_->count; ++i) {
      if (static_cast<int>(i) == activity->selected_) continue;  // Selected asset uses the real flow.
      auto result = AssetDownloadService::download(*activity->catalog_, activity->catalog_->entries[i],
                                                   activity->endpoints_, false);
      if (result == Result::OverwriteRequired && activity->kind_ == Kind::Translation &&
          !std::strcmp(activity->catalog_->entries[i].code, originalUi())) {
        // --language-file seeds a manual source for the initial RTL locale.
        // Assert refusal first, then explicitly approve replacement of that
        // known fixture while its already-active flash mapping stays pinned.
        AssetDownloadService::Source manual;
        require(AssetDownloadService::discover(activity->kind_, activity->catalog_->entries[i], manual) == Result::Ok &&
                    manual.status == Status::Different && !std::strcmp(manual.path, "/.crossink/languages/smoke.yaml"),
                "overwrite request was not the seeded manual language");
        unchanged(false);
        result = AssetDownloadService::download(*activity->catalog_, activity->catalog_->entries[i],
                                                activity->endpoints_, true);
      }
      if (result != Result::Ok)
        LOG_ERR("SMOKE", "Asset %s: %s", activity->catalog_->entries[i].code, AssetDownloadService::resultName(result));
      require(result == Result::Ok, "catalog asset download failed");
      delivered(*activity->catalog_, activity->catalog_->entries[i]);
    }
  }
  // Hold back one companion so the unattended install stops at a Retry prompt,
  // where the service-level preservation checks can run before activation.
  static std::filesystem::path heldCompanion(bool hold) {
    const auto root = std::filesystem::path(std::getenv("CROSSINK_SIM_HTTP_MOCK_ROOT")) / "snapshots";
    std::ifstream list("asset-expected/translation-DE.txt");
    std::string relative;
    if (!std::getline(list, relative) || !std::filesystem::is_directory(root)) return {};
    for (const auto& snapshot : std::filesystem::directory_iterator(root)) {
      const auto file = snapshot.path() / relative;
      const auto held = file.string() + ".test-held";
      if (hold && std::filesystem::exists(file)) {
        std::filesystem::rename(file, held);
        return file;
      }
      if (!hold && std::filesystem::exists(held)) {
        std::filesystem::rename(held, file);
        return file;
      }
    }
    return {};
  }
  static uint8_t germanTranslationIndex() {
    for (uint8_t i = 1; i < getLanguageCount(); ++i)
      if (!std::strcmp(LANGUAGE_CODES[i], "DE")) return i;
    require(false, "German missing from UI language table");
    return 0;
  }
  // Opens Settings > Download Languages and taps German. An Install row boots the network
  // directly; an installed/active row opens a popup whose Update does the same.
  void driveSettings(bool installed, const char* nextStage) {
    if (settingsPhase == 0) {
      activityManager.pushActivity(makeUniqueNoThrow<SettingsActivity>(renderer, mappedInputManager));
      settingsPhase = 1;
      return;
    }
    auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
    if (!settings) return;
    const int row = germanTranslationIndex();
    if (settingsPhase == 1) {
      settings->openLanguageDownloads();
      navLeft = row - static_cast<int>(I18n::languageFromCode(I18N.getCode()));
      settingsPhase = 2;
      return;
    }
    if (!settings->optionPopup.isActive()) return;
    if (settingsPhase == 2) {
      require(settings->languageDownloadsOpen && settings->languageCatalog, "Downloads did not show languages");
      require(WiFi.getMode() == WIFI_OFF, "Downloads connected Wi-Fi");
      require(std::strstr(settings->optionPopup.simulatorOptionLabel(0), tr(STR_HYPHENATION_BUILT_IN)),
              "English is not Built in");
      require(std::strstr(settings->optionPopup.simulatorOptionLabel(row),
                          installed ? tr(STR_ASSET_CURRENT) : tr(STR_HYPHENATION_INSTALL_ACTION)),
              "German row has the wrong status");
      captureMatrix(installed ? "assets-language-downloads-installed" : "assets-language-downloads-install");
      if (mappedInputManager.hasTouchHardware()) {
        if (!installed) setenv("CROSSINK_ASSET_SMOKE_STAGE", nextStage, 1);
        tapPopup(settings->optionPopup, row);
        settingsPhase = 3;
      } else if (navLeft) {
        tap(navLeft > 0 ? Button::Down : Button::Up);
        navLeft += navLeft > 0 ? -1 : 1;
      } else {
        if (!installed) setenv("CROSSINK_ASSET_SMOKE_STAGE", nextStage, 1);
        tap(Button::Confirm);
        settingsPhase = 3;
      }
      return;
    }
    if (!installed || settings->languageDownloadsOpen || settings->pendingLanguageDownloadAction ||
        std::strcmp(settings->optionPopup.simulatorOptionLabel(0), tr(STR_UPDATE)))
      return;  // Install rows have no further step; wait for the actions popup otherwise.
    if (settingsPhase == 3) {
      require(!std::strcmp(settings->optionPopup.simulatorOptionLabel(1), tr(STR_CANCEL)),
              "active language does not offer Update and Cancel");
      captureMatrix("assets-language-active-actions");
      if (mappedInputManager.hasTouchHardware()) {
        setenv("CROSSINK_ASSET_SMOKE_STAGE", nextStage, 1);
        tapPopup(settings->optionPopup, 0);
        settingsPhase = 5;
      } else {
        tap(Button::Up);  // Default Cancel -> Update.
        settingsPhase = 4;
      }
    } else if (settingsPhase == 4) {
      setenv("CROSSINK_ASSET_SMOKE_STAGE", nextStage, 1);
      tap(Button::Confirm);
      settingsPhase = 5;
    }
  }
  void tick() {
    if (inputTick()) return;
    if (initialStage < 0) initialStage = stage();
    const int bootStage = initialStage;
    require(bootStage != 9, "identical active translation unexpectedly restarted");
    if (bootStage == 0) {
      // Exercise the public manager before entering the online downloader.
      if (managerPhase == 0 && activityManager.isHomeActivity()) {
        activityManager.pushActivity(makeUniqueNoThrow<HyphenationManagerActivity>(renderer, mappedInputManager));
        managerPhase = 1;
        return;
      }
      if (managerPhase == 5 && activityManager.isHomeActivity()) managerPhase = 6;
      if (managerPhase > 0 && managerPhase < 6) {
        auto* manager = dynamic_cast<HyphenationManagerActivity*>(activityManager.simulatorCurrentActivity());
        if (!manager) return;
        if (managerPhase == 1) {
          require(!manager->simulatorOptionDisabled(0), "built-in English is crossed out");
          require(std::strstr(manager->simulatorLabel(2), tr(STR_HYPHENATION_INSTALL_ACTION)) &&
                      !std::strstr(manager->simulatorLabel(2), tr(STR_ASSET_USE)),
                  "missing pack has no Install action");
          captureMatrix("hyphenation-manager-missing");
          tapPopup(manager->popup_, 0);
          managerPhase = 2;
        } else if (managerPhase == 2) {
          require(manager->popup_.isActive() && manager->pending_ == HyphenationManagerActivity::Pending::None &&
                      !HyphenationPackStore::isInstalled("en") && WiFi.getMode() == WIFI_OFF,
                  "English selection triggered an action");
          tap(Button::Back);
          managerPhase = 5;
        } else {
          managerPhase = 6;
        }
        return;
      }
      if (managerPhase >= 7) {
        // Selecting a missing pack goes straight to the network boot, with no confirmation popup.
        auto* manager = dynamic_cast<HyphenationManagerActivity*>(activityManager.simulatorCurrentActivity());
        if (!manager || managerPhase > 7 || manager->pending_ != HyphenationManagerActivity::Pending::None) return;
        require(std::strstr(manager->simulatorLabel(2), tr(STR_HYPHENATION_INSTALL_ACTION)),
                "German is not offered as Install");
        setenv("CROSSINK_ASSET_SMOKE_STAGE", "1", 1);
        tapPopup(manager->popup_, 2);
        managerPhase = 8;
        return;
      }
      if (offlinePhase == 0 && activityManager.isHomeActivity()) {
        activityManager.pushActivity(makeUniqueNoThrow<SettingsActivity>(renderer, mappedInputManager));
        offlinePhase = 1;
        return;
      }
      if (offlinePhase > 0 && offlinePhase < 4) {
        auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (!settings) return;
        if (offlinePhase == 1) {
          settings->openLanguageDownloads();
          require(settings->languageDownloadsOpen && settings->languageCatalog && settings->optionPopup.isActive(),
                  "offline Downloads did not show local catalog");
          require(std::strstr(settings->optionPopup.simulatorOptionLabel(0), tr(STR_HYPHENATION_BUILT_IN)) &&
                      std::strstr(settings->optionPopup.simulatorOptionLabel(germanTranslationIndex()),
                                  tr(STR_HYPHENATION_INSTALL_ACTION)),
                  "offline Downloads do not list built-in English and installable languages");
          require(WiFi.getMode() == WIFI_OFF, "offline Downloads connected Wi-Fi");
          captureMatrix("assets-offline-language-sources");
          tap(Button::Back);
          offlinePhase = 2;
        } else {
          require(!settings->languageDownloadsOpen && !settings->optionPopup.isActive(), "offline Cancel failed");
          activityManager.popActivity();
          offlinePhase = 4;
        }
        return;
      }
      if (!activityManager.isHomeActivity()) return;
      require(std::getenv("CROSSINK_SIM_HTTP_MOCK_ROOT"), "requires isolated HTTP mock root");
      setenv("CROSSINK_ASSET_SMOKE_UI", I18N.getCode(), 1);
      setenv("CROSSINK_ASSET_SMOKE_UI_GENERATION", std::to_string(I18N.getGeneration()).c_str(), 1);
      setenv("CROSSINK_ASSET_SMOKE_SETTINGS_GENERATION", std::to_string(SETTINGS.languageCacheGeneration).c_str(), 1);
      require(WIFI_STORE.addCredential("asset-smoke", ""), "cannot save synthetic open Wi-Fi fixture");
      WIFI_STORE.setLastConnectedSsid("asset-smoke");
      APP_STATE.openEpubPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
      require(APP_STATE.saveToFile(), "cannot save book context");
      std::ofstream("fs_/.crosspoint/asset-progress-sentinel") << "book progress retained\n";
      const auto cache = Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint");
      require(Storage.ensureDirectoryExists(cache.c_str()), "cannot create EPUB progress fixture");
      FsFile progressFile;
      require(Storage.openFileForWrite("SMOKE", cache + "/progress.bin", progressFile),
              "cannot open EPUB progress fixture");
      const uint8_t position[] = {1, 0, 2, 0, 10, 0, 0xd2, 4, 0, 0};
      require(progressFile.write(position, sizeof(position)) == sizeof(position) && progressFile.sync() &&
                  progressFile.close(),
              "cannot persist EPUB progress fixture");
      activityManager.pushActivity(makeUniqueNoThrow<HyphenationManagerActivity>(renderer, mappedInputManager, "de"));
      managerPhase = 7;
      return;
    }
    if (bootStage == 6) {
      if (!activityManager.isHomeActivity()) return;
      unchanged(false);
      require(!std::strcmp(SETTINGS.languageCode, originalUi()), "failed save selected a different language");
      LOG_INF("SMOKE",
              "Simulator smoke test passed: language save failure, Cancel, mutation guard and explicit restart "
              "preserve active language and progress");
      std::_Exit(0);
    }
    if (bootStage == 8) {
      if (!activityManager.isHomeActivity()) return;
      const char* previous = std::getenv("CROSSINK_ASSET_SMOKE_UPDATED_GENERATION");
      require(previous && !std::strcmp(I18N.getCode(), "DE") &&
                  I18N.getGeneration() == std::strtoull(previous, nullptr, 10) + 1 &&
                  SETTINGS.languageCacheGeneration == I18N.getGeneration(),
              "changed same-code translation did not activate after unchanged no-op");
      checkProgress();
      success();
    }
    if (bootStage == 2 && updatePhase > 0) {
      // Installed packs offer Update / Delete / Cancel; Update goes straight to the network boot.
      auto* manager = dynamic_cast<HyphenationManagerActivity*>(activityManager.simulatorCurrentActivity());
      if (!manager || updatePhase >= 5 || manager->pending_ != HyphenationManagerActivity::Pending::None ||
          !manager->popup_.isActive())
        return;
      if (updatePhase == 1) {
        require(std::strstr(manager->simulatorLabel(2), tr(STR_INSTALLED)), "installed pack is not labelled Installed");
        captureMatrix("hyphenation-manager-installed");
        tapPopup(manager->popup_, 2);
        updatePhase = 2;
        return;
      }
      if (std::strcmp(manager->simulatorOptionLabel(0), tr(STR_UPDATE))) return;  // List still showing.
      if (updatePhase == 2) {
        require(!std::strcmp(manager->simulatorOptionLabel(1), tr(STR_DELETE)) &&
                    !std::strcmp(manager->simulatorOptionLabel(2), tr(STR_CANCEL)),
                "installed pack does not offer Update, Delete and Cancel");
        captureMatrix("hyphenation-manager-actions");
      }
      if (mappedInputManager.hasTouchHardware() || updatePhase == 4) {
        setenv("CROSSINK_ASSET_SMOKE_STAGE", "7", 1);
        if (mappedInputManager.hasTouchHardware())
          tapPopup(manager->popup_, 0);
        else
          tap(Button::Confirm);
        updatePhase = 5;
      } else {
        tap(Button::Up);  // Default Cancel -> Delete -> Update.
        ++updatePhase;
      }
      return;
    }
    if (bootStage == 4) {
      if (settingsPhase == 0) {
        if (!activityManager.isHomeActivity()) return;
        require(HyphenationPackStore::isInstalled("de"), "hyphenation activation did not persist");
        require(!std::strcmp(I18N.getCode(), "DE"), "translation activation did not persist");
        checkProgress();
        capture("assets-language-home");
        // Seed an edited source so the next stage meets the overwrite prompt.
        std::ofstream("fs_/.crossink/languages/DE.yaml", std::ios::binary | std::ios::app) << "\n# local edit\n";
      }
      driveSettings(true, "12");  // Active language: Update goes straight to the network boot.
      return;
    }
    if (bootStage == 2) {
      if (!activityManager.isHomeActivity()) return;
      require(HyphenationPackStore::isInstalled("de"), "hyphenation activation did not persist");
      checkProgress();
      require(!std::strcmp(I18N.getCode(), originalUi()), "hyphenation activation changed UI locale");
      capture("assets-hyphenation-home");
      activityManager.pushActivity(makeUniqueNoThrow<HyphenationManagerActivity>(renderer, mappedInputManager, "de"));
      updatePhase = 1;
      return;
    }
    const bool expectTranslationRejection =
        bootStage == 3 && std::getenv("CROSSINK_ASSET_SMOKE_EXPECT_TRANSLATION_REJECTION");
    const bool expectEmpty = bootStage == 1 && std::getenv("CROSSINK_ASSET_SMOKE_EMPTY_CATALOG");
    if (expectEmpty && phase == 1 && activityManager.isHomeActivity()) {
      unchanged(true);
      LOG_INF("SMOKE", "Simulator smoke test passed: empty catalog exits to Home without transfer or activation");
      std::_Exit(0);
    }
    if (expectTranslationRejection && phase == 1 && activityManager.isHomeActivity()) {
      unchanged(false);
      LOG_INF("SMOKE", "Asset smoke: obsolete translation catalog rejected; Cancel returned Home without activation");
      success();
    }
    auto* activity = dynamic_cast<AssetDownloadActivity*>(activityManager.simulatorCurrentActivity());
    if (bootStage == 1 && !expectEmpty) {
      // Install runs unattended: catalog -> discover -> download -> activate -> restart.
      if (activity && phase == 0) {
        setenv("CROSSINK_ASSET_SMOKE_STAGE", "2", 1);
        phase = 1;
      }
      require(!(phase == 1 && activityManager.isHomeActivity()), "hyphenation install exited without activating");
      require(!(activity && ready(*activity)), "hyphenation install stopped at a prompt");
      return;
    }
    if (bootStage == 11 && settingsPhase > 0) {
      driveSettings(false, "3");
      return;
    }
    if (bootStage == 7 || bootStage == 10 || bootStage == 11) {
      if (!activity) {
        if (phase < (bootStage == 11 ? 3u : 1u) || !activityManager.isHomeActivity()) return;
        require(HyphenationPackStore::isInstalled("de"), "hyphenation pack lost");
        checkProgress();
        const bool edited = localHyphenationSource().find("# local edit") != std::string::npos;
        if (bootStage == 7) {
          std::ofstream("fs_/.crossink/hyphenation/hyph-de.cphyph", std::ios::binary | std::ios::app)
              << "\n# local edit\n";
          startNetwork(Kind::Hyphenation, "10");
        } else if (bootStage == 10) {
          require(edited, "overwrite Cancel replaced the edited source");
          startNetwork(Kind::Hyphenation, "11");
        } else {
          require(!edited, "approved overwrite kept the edited source");
          if (std::getenv("CROSSINK_ASSET_SMOKE_HYPHENATION_ONLY")) success();
          if (!std::getenv("CROSSINK_ASSET_SMOKE_EXPECT_TRANSLATION_REJECTION")) heldCompanion(true);
          driveSettings(false, "3");  // Install row: straight to the network boot.
        }
        return;
      }
      if (!ready(*activity) || (bootStage == 11 ? phase >= 3 : phase != 0 && bootStage != 11)) return;
      const bool info = activity->simulatorLastError_ == StrId::STR_ASSET_CURRENT;
      if (bootStage == 10 || (bootStage == 11 && phase < 2)) {
        require(!info && !std::strcmp(activity->popup_.simulatorOptionLabel(0), tr(STR_CANCEL)) &&
                    !std::strcmp(activity->popup_.simulatorOptionLabel(1), tr(STR_DOWNLOAD)),
                "edited source did not ask before overwrite");
        require(HyphenationPackStore::isInstalled("de"), "overwrite prompt changed active hyphenation");
        if (bootStage == 10) {
          captureMatrix("assets-hyphenation-overwrite");
          tapOption(*activity, 0);
          phase = 1;
        } else if (!mappedInputManager.hasTouchHardware() && phase == 0) {
          tap(Button::Down);
          phase = 1;
        } else {
          tapOption(*activity, 1);
          phase = 2;
        }
        return;
      }
      if (bootStage == 11 && phase != 2) return;
      require(info && activity->catalogReady_, "Unchanged install not reported");
      require(HyphenationPackStore::isInstalled("de"), "unchanged install removed the pack");
      checkProgress();
      if (bootStage == 7) {
        // Every other catalog pack still downloads, verifies and publishes through the real service.
        for (size_t i = 0; i < activity->catalog_->count; ++i) {
          if (static_cast<int>(i) == activity->selected_) continue;
          require(AssetDownloadService::download(*activity->catalog_, activity->catalog_->entries[i],
                                                 activity->endpoints_, false) == Result::Ok,
                  "catalog asset download failed");
          delivered(*activity->catalog_, activity->catalog_->entries[i]);
        }
        delivered(*activity->catalog_, activity->catalog_->entries[activity->selected_]);
        captureMatrix("assets-hyphenation-unchanged");
      }
      tapOption(*activity, 0);  // OK exits to Home.
      phase = bootStage == 11 ? 3 : 1;
      return;
    }
    if (bootStage == 3 && !expectTranslationRejection) {
      // Install runs unattended. A held-back companion makes it stop at Retry, so the
      // service-level preservation checks can run before the real activation.
      if (!activity) {
        require(!(phase > 0 && phase != 3 && activityManager.isHomeActivity()),
                "translation install exited without activating");
        return;
      }
      if (!ready(*activity)) return;
      const bool saveFailure = std::getenv("CROSSINK_ASSET_SMOKE_SAVE_FAILURE");
      if (phase == 0) {
        require(activity->catalogReady_ && activity->simulatorLastError_ == StrId::STR_DOWNLOAD_FAILED &&
                    !std::strcmp(activity->popup_.simulatorOptionLabel(1), tr(STR_RETRY)),
                "held companion did not stop the install at Retry");
        heldCompanion(false);
        unchanged(false);
        companionChecks(*activity, true);
        downloadCatalog(activity);
        unchanged(false);
        require(download(*activity) == Result::Ok, "download failed after companion restored");
        require(discover(*activity) == Result::Ok && source.status == Status::Current, "download was not current");
        delivered(*activity->catalog_, activity->catalog_->entries[activity->selected_]);
        transactionChecks(*activity);
        preservationChecks(*activity);
        if (saveFailure) {
          // A directory at the atomic temporary-file path forces the real SD
          // writer to fail, without changing the previously saved settings.
          require(std::filesystem::create_directory("fs_/.crosspoint/crossink-settings.json.tmp"),
                  "cannot create settings write fault");
          // The simulator removes stale temporary paths before opening them;
          // keep this directory nonempty so that cleanup also fails.
          std::ofstream blocker("fs_/.crosspoint/crossink-settings.json.tmp/blocker");
          blocker << "forced settings write failure";
          blocker.close();
          require(!blocker.fail(), "cannot populate settings write fault");
        }
        setenv("CROSSINK_ASSET_SMOKE_STAGE", saveFailure ? "6" : "4", 1);
        if (mappedInputManager.hasTouchHardware()) {
          tapOption(*activity, 1);
          phase = 2;
        } else {
          tap(Button::Down);  // Cancel -> Retry.
          phase = 1;
        }
        return;
      }
      if (phase == 1) {
        tapOption(*activity, 1);
        phase = 2;
        return;
      }
      if (phase == 3 || activity->simulatorLastError_ == StrId::STR_DOWNLOAD_FAILED) return;  // Retry still pending.
      require(saveFailure && activity->simulatorLastError_ == StrId::STR_LANGUAGE_SAVE_FAILED,
              "translation install stopped at a prompt");
      if (phase == 5) {
        tapOption(*activity, 1);
        phase = 3;
        return;
      }
      require(activity->restartRequired_, "settings failure did not expose restart recovery");
      unchanged(false);
      require(!std::strcmp(SETTINGS.languageCode, originalUi()), "failed save did not roll settings back");
      language_cache::Installed ignored;
      require(I18N.prepare(activity->source_.path, ignored) == language_cache::Result::RestartRequired,
              "second flash mutation allowed after failed settings save");
      captureMatrix("assets-language-save-failure");
      require(std::filesystem::remove("fs_/.crosspoint/crossink-settings.json.tmp/blocker") &&
                  std::filesystem::remove("fs_/.crosspoint/crossink-settings.json.tmp"),
              "cannot clear write fault");
      if (mappedInputManager.hasTouchHardware()) {
        tapOption(*activity, 1);  // Explicit OK restarts using the previous saved locale.
        phase = 3;
      } else {
        tap(Button::Down);  // Cancel -> OK.
        phase = 5;
      }
      return;
    }
    if (bootStage == 12 || bootStage == 13) {
      // DE is active and its SD copy was edited: the install must ask before replacing it.
      if (activity && !seen) {
        seen = true;
        activeFlashBefore = flashBytes();
        activeGenerationBefore = I18N.getGeneration();
        setenv("CROSSINK_ASSET_SMOKE_STAGE", "9", 1);  // Unchanged must never restart.
      }
      if (!activity) {
        if (!seen || !activityManager.isHomeActivity()) return;
        require(bootStage == 12 && phase == 1, "translation install exited without the expected prompt");
        std::ifstream edited("fs_/.crossink/languages/DE.yaml");
        std::string yaml{std::istreambuf_iterator<char>(edited), std::istreambuf_iterator<char>()};
        require(yaml.find("# local edit") != std::string::npos, "overwrite Cancel replaced the edited source");
        require(!std::strcmp(I18N.getCode(), "DE"), "overwrite Cancel changed the language");
        checkProgress();
        startNetwork(Kind::Translation, "13");
        return;
      }
      if (!ready(*activity) || phase == 1 || phase == 3) return;
      const bool info = activity->simulatorLastError_ == StrId::STR_ASSET_CURRENT;
      if (phase == 0) {
        require(!info && !std::strcmp(activity->popup_.simulatorOptionLabel(0), tr(STR_CANCEL)) &&
                    !std::strcmp(activity->popup_.simulatorOptionLabel(1), tr(STR_DOWNLOAD)),
                "edited source did not ask before overwrite");
        if (bootStage == 12) {
          captureMatrix("assets-language-overwrite");
          tapOption(*activity, 0);
          phase = 1;
        } else if (!mappedInputManager.hasTouchHardware() && phase == 0) {
          tap(Button::Down);
          phase = 4;
        } else {
          tapOption(*activity, 1);
          phase = 2;
        }
        return;
      }
      if (phase == 4) {
        tapOption(*activity, 1);
        phase = 2;
        return;
      }
      if (!info) return;
      require(phase == 2 && !activity->restartRequired_ && I18N.getGeneration() == activeGenerationBefore &&
                  SETTINGS.languageCacheGeneration == activeGenerationBefore && flashBytes() == activeFlashBefore,
              "identical active translation rewrote flash, advanced generation or requested restart");
      checkProgress();
      captureMatrix("assets-language-unchanged");
      // Changing real source metadata must still prepare successfully after the
      // no-op, proving it did not consume this boot's single flash mutation.
      const auto path = std::string("fs_") + activity->source_.path;
      std::ifstream input(path, std::ios::binary);
      std::string yaml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      input.close();
      const auto name = yaml.find("_language_name:");
      const auto end = yaml.find('\n', name);
      require(name != std::string::npos && end != std::string::npos, "language metadata fixture missing");
      yaml.replace(name, end - name, "_language_name: \"Deutsch Updated\"");
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << yaml;
      output.close();
      require(!output.fail(), "cannot update same-code source metadata");
      setenv("CROSSINK_ASSET_SMOKE_UPDATED_GENERATION", std::to_string(activeGenerationBefore).c_str(), 1);
      setenv("CROSSINK_ASSET_SMOKE_STAGE", "8", 1);
      phase = 3;
      activity->pending_ = AssetDownloadActivity::Pending::Activate;
      return;
    }
    if (!activity || !ready(*activity)) return;
    if (expectEmpty) {
      require(!activity->catalogReady_ && activity->catalog_->count == 0 &&
                  activity->simulatorLastError_ == StrId::STR_ASSET_UNAVAILABLE,
              "empty catalog reported an invalid catalog or network failure");
      require(!Storage.exists("/.crossink/hyphenation/hyph-de.cphyph"), "empty catalog downloaded a source");
      unchanged(true);
      require(phase == 0, "empty catalog exit did not return Home");
      captureMatrix("assets-empty-catalog");
      ++phase;
      tap(Button::Confirm);
      return;
    }
    if (expectTranslationRejection) {
      require(!activity->catalogReady_ && activity->simulatorLastError_ == StrId::STR_ASSET_CATALOG_INVALID,
              "obsolete translation did not reach catalog validation rejection");
      require(!Storage.exists("/.crossink/languages/DE.yaml"), "rejected catalog downloaded a source");
      unchanged(false);
      require(phase == 0, "rejected catalog Cancel did not return Home");
      capture("assets-obsolete-translation-rejected");
      ++phase;
      tap(Button::Confirm);  // Error popup defaults to Cancel rather than Retry.
      return;
    }
  }
};

bool tickAssetDownloadSmokeTest() {
  if (!std::getenv("CROSSINK_SIMULATOR_SMOKE_ASSET_DOWNLOADS")) return false;
  static AssetDownloadSmokeTest smoke;
  smoke.tick();
  return true;
}
#endif
