#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

class DictionarySelectActivity final : public Activity {
 public:
  DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath = "",
                           bool disableCurrentSelection = false, bool temporarySelection = false,
                           std::string displayedDictionary = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Active SD card root directory for dictionaries (resolved in scanDictionaries()).
  std::string dictRoot;

  // Discovered dictionary folder names and file stems (parallel vectors, excluding "None").
  // e.g. dictFolders[i] = "dict-en-en", dictStems[i] = "dict-data"
  // folderForIndex() combines them with dictRoot into the full base path used for file access.
  std::vector<std::string> dictFolders;
  std::vector<std::string> dictStems;

  // Index into the full list including "None" at position 0.
  int selectedIndex = 0;
  int totalItems = 0;

  // Suppresses the Confirm release that bleeds through from the parent activity launch.
  bool ignoreNextConfirmRelease = false;

  // Non-empty when launched from reader menu (per-book override mode).
  std::string bookCachePath;
  // Active per-book dictionary path loaded on enter; empty = "Use Global".
  std::string currentBookDictPath;
  // Effective dictionary used by the current reader lookup. Non-empty only when
  // launched as a switcher from a definition view.
  std::string currentEffectiveDictPath;
  // Augmented "Use Global" label showing the global dict name, e.g. "Use Global (dict-en-en)".
  // Only populated in per-book mode.
  std::string useGlobalLabel;
  bool disableCurrentSelection = false;
  // Lookup switchers return the chosen path without writing book/global settings.
  bool temporarySelection = false;

  ButtonNavigator buttonNavigator;
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool initialViewportPending = true;
  OptionPopup optionPopup;
  bool popupSelectionMade = false;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void finishSelection();
  bool usesPopup() const { return !bookCachePath.empty() && !disableCurrentSelection; }

  // Scans the first available dictionary root directory on the SD card and populates dictFolders.
  void scanDictionaries();

  // Returns the folder path for a given list index (0 = None → empty string).
  std::string folderForIndex(int index) const;

  // Returns the dictionary that selecting an item would make active. In per-book
  // mode, index 0 means "Use Global" and resolves to the global dictionary path.
  std::string effectiveFolderForIndex(int index) const;

  bool rowIsDisabled(int index) const;
  int firstSelectableIndexFrom(int start) const;

  // Returns the display name for a given list index.
  const char* nameForIndex(int index) const;

  // Applies the currently highlighted selection to settings and Dictionary.
  bool applySelection();
};
