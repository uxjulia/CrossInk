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
// Official builds opt into their own host. Unconfigured builds use the
// matching GitHub release for both the catalog and individual font files.
#ifndef FONT_DOWNLOAD_BASE_URL
#define FONT_DOWNLOAD_BASE_URL "https://github.com/uxjulia/crossink-fonts/releases/download/"
#define FONT_DOWNLOAD_USE_GITHUB_RELEASES
#endif
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_DOWNLOAD_CATALOG_BASE_URL                             \
  FONT_DOWNLOAD_BASE_URL "sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/"
#define FONT_MANIFEST_URL FONT_DOWNLOAD_CATALOG_BASE_URL "fonts.json"
#endif

class FontDownloadActivity : public Activity {
#ifdef SIMULATOR
  friend struct ScreenCalibrationSmokeTest;
#endif
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
    // The downloaded manifest can contain hundreds of file names. They all
    // point into manifestStringArena_ so the catalog does not fragment the
    // C3 heap with one std::string allocation per entry.
    const char* name = "";
    size_t size = 0;
    uint32_t crc32 = 0;
    uint8_t pointSize = 0;
  };

  struct ManifestFamily {
    const char* name = "";
    std::string installName;
    const char* description = "";
    const char* languages = "";
    size_t fileStart = 0;
    size_t fileCount = 0;
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
  // One activity-owned allocation for all manifest labels and file names.
  // It remains alive while an update batch uses a copied ManifestFamily.
  std::unique_ptr<char[]> manifestStringArena_;
  size_t manifestStringArenaUsed_ = 0;
  size_t manifestStringArenaCapacity_ = 0;
  std::unique_ptr<ManifestFile[]> manifestFiles_;
  std::unique_ptr<ManifestFamily[]> manifestFamilies_;
  size_t manifestFamilyCount_ = 0;
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
  bool internManifestString(const char* text, const char*& out);
  bool rebuildListItems();
  const SdCardFontFamilyInfo* findInstalledFamilyCandidate(const char* familyName) const;
  bool installedFilesMatch(const ManifestFamily& family, bool& hasUpdate,
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
};
