#pragma once
#include <memory>

#include "EpubReaderActivity.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  struct EpubOpenResult {
    std::unique_ptr<Epub> epub;
    EpubReaderActivity::BookReaderSettingsData readerSettings;
    Epub::OpenFailure failure = Epub::OpenFailure::InvalidOrUnreadable;
  };

  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  bool suppressInitialBackRelease = false;
  bool allowFastInitialRefresh = false;
  bool cleanImageBaseOnEntry = false;
  bool keepWakeFrame = false;
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  EpubOpenResult loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  BookLoadingFeedback openingFeedback;

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub, EpubReaderActivity::BookReaderSettingsData readerSettings);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);
  void queueEpubOpenAlert(Epub::OpenFailure failure);

  void onGoBack();
  int initialRefreshCountdown() const;

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          bool suppressInitialBackRelease = false, bool allowFastInitialRefresh = false,
                          bool cleanImageBaseOnEntry = false, bool keepWakeFrame = false)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        suppressInitialBackRelease(suppressInitialBackRelease),
        allowFastInitialRefresh(allowFastInitialRefresh),
        cleanImageBaseOnEntry(cleanImageBaseOnEntry),
        keepWakeFrame(keepWakeFrame) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
