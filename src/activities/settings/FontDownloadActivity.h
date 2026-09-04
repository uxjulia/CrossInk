#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "util/ButtonNavigator.h"

struct Rect;

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

#ifndef FONT_MANIFEST_URL
// Default hosted SD-font manifest. Use plain HTTP for this public S3 bucket:
// HTTPS stalls inside esp_http_client on ESP32-C3, and downloaded .cpfont files
// are still validated by CRC before install. The versioned prefix must stay in
// sync with .github/workflows/release-fonts.yml and cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                    \
  "http://crossink-fonts.s3.us-east-1.amazonaws.com/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
    uint8_t pointSize = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string installName;
    std::string description;
    std::string languages;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh_;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  // Built once after each manifest load. The renderer borrows these pointers,
  // so keeping them activity-owned avoids heap growth on every redraw.
  std::unique_ptr<freeink::ui::ListItem[]> listItems_;
  size_t listItemCapacity_ = 0;
  size_t listItemCount_ = 0;
  char updateAllLabel_[96] = {};
  int selectedIndex_ = 0;
  ManifestFamily retryFamily_;
  bool hasRetryFamily_ = false;
  bool manifestReloadNeeded_ = false;
  std::string activeDownloadFamilyName_;
  bool fontsChanged_ = false;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadAttempt_ = 0;
  int downloadAttemptTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  std::string errorHint_;
  bool cancelRequested_ = false;
  // Set when a blocking download consumed Home; exit only after its file and
  // network resources have unwound.
  bool goHomeRequested_ = false;

  // FreeInkApp hosts the family list (themed rows, touch routing); the other
  // states keep their legacy centered-text rendering.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget_;  // must precede `app_`: the app holds a reference to it
  UiApp app_;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady_{false};
  int visibleRows_ = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex_ = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void activateSelected();
  bool pollCancelInput(bool includeDownloadScreenButton);

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  bool rebuildListItems();
  const SdCardFontFamilyInfo* findInstalledFamilyCandidate(const char* familyName) const;
  bool installedFilesMatch(const char* familyName, const std::vector<ManifestFile>& files, bool& hasUpdate,
                           std::string* resolvedFamilyName = nullptr) const;
  void resolveInstalledFamilyName(ManifestFamily& family) const;
  void clearManifestFamilies();
  void downloadFamily(ManifestFamily& family);
  void returnToFamilyList();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalUpdateSize() const;
  static void formatSize(size_t bytes, char* buffer, size_t bufferSize);
};
