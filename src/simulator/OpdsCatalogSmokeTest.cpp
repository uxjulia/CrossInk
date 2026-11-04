#ifdef SIMULATOR
#include "OpdsCatalogSmokeTest.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/settings/OpdsSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

struct OpdsCatalogSmokeTest {
  using State = OpdsBookBrowserActivity::BrowserState;
  using Request = OpdsBookBrowserActivity::DownloadRequest;
  static constexpr const char* ORIGINAL_TITLE = "Original requested book";

  static void require(bool ok, const char* message) {
    if (ok) return;
    LOG_ERR("SMOKE", "OPDS catalog: %s", message);
    std::_Exit(2);
  }

  static void render() {
    require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "render failed");
  }

  static void start(OpdsBookBrowserActivity& browser) {
    {
      RenderLock lock;
      require(browser.entries && browser.entryCount == 3, "fixture catalog missing");
      browser.selectorIndex = 2;
      browser.topIndex = 0;
      browser.descriptionLines.emplace_back("Allocated preview text that must be released before TLS");
    }
    browser.requestDownload(browser.entries[2]);
    RenderLock lock;
    require(browser.state == State::DOWNLOADING, "download did not start");
    require(!browser.entries && browser.entryCount == 0, "catalog retained during download");
    require(browser.descriptionLines.empty() && browser.descriptionLines.capacity() == 0, "preview retained");
    require(browser.statusMessage == "The Time Machine", "download title references freed catalog");
  }

  static void restored(OpdsBookBrowserActivity& browser) {
    render();
    RenderLock lock;
    require(browser.state == State::BROWSING && browser.entries && browser.entryCount == 3, "catalog not restored");
    require(browser.selectorIndex == 2 && browser.topIndex == 0, "browsing position lost");
    require(browser.currentPath == "/fiction" && browser.navigationHistory.size() == 1 &&
                browser.navigationHistory[0].empty() && !browser.searchTemplate.empty(),
            "navigation/search lost");
    require(!browser.catalogReleasedForDownload, "release state not reset");
  }

  static void outcomes(OpdsBookBrowserActivity& browser) {
    {
      RenderLock lock;
      browser.currentPath = "/fiction";
      browser.navigationHistory.emplace_back("");
    }
    browser.fetchFeed(browser.currentPath);
    start(browser);
    browser.finishDownload({}, HttpDownloader::OK, "/books/downloaded.epub");
    restored(browser);

    start(browser);
    OpdsBookBrowserActivity::onCancelEvent({}, &browser);
    browser.loop();
    restored(browser);

    for (const auto error : {HttpDownloader::HTTP_ERROR, HttpDownloader::INSUFFICIENT_SPACE}) {
      start(browser);
      browser.finishDownload({}, error, "");
      {
        RenderLock lock;
        require(browser.state == State::ERROR && !browser.entries && browser.catalogReleasedForDownload,
                "failed download lost error/retry state");
      }
      // Reconnecting after a failed transfer must restore the original view too.
      browser.onWifiSelectionComplete(true);
      restored(browser);
    }
  }

  static void collision(OpdsBookBrowserActivity& browser) {
    Request request{"https://opds.example/books/original.epub", ORIGINAL_TITLE, "/books/fallback.epub"};
    browser.downloadBook(request);
    browser.finishDownload(std::move(request), HttpDownloader::FILE_EXISTS, "/books/server-name.epub");
  }

  static void closeConfirmation(bool cancelled) {
    require(activityManager.isCurrentActivityNamed("Confirmation"), "overwrite confirmation missing");
    render();
    ActivityResult result;
    result.isCancelled = cancelled;
    activityManager.simulatorCurrentActivity()->setResult(std::move(result));
    activityManager.simulatorCurrentActivity()->finish();
  }

  static void capture(const char* name) {
    const char* output = std::getenv("CROSSINK_SIMULATOR_OPDS_CAPTURES");
    if (!output) return;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    require(image != nullptr, "cannot capture OPDS settings");
    const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
    std::fprintf(image, "P5\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    std::fclose(image);
  }

  static void finishWith(ActivityResult result) {
    activityManager.simulatorCurrentActivity()->setResult(std::move(result));
    activityManager.simulatorCurrentActivity()->finish();
  }

  static bool settingsTick() {
    static int phase = 0;
    static OpdsSettingsActivity* editor = nullptr;
    switch (phase++) {
      case 0: {
        OpdsServer server;
        server.name = "Calibre";
        server.url = "https://calibre.example/opds";
        require(OPDS_STORE.addServer(server), "cannot save folder fixture");
        std::snprintf(SETTINGS.opdsDownloadFolder, sizeof(SETTINGS.opdsDownloadFolder), "/Books");
        auto activity = std::make_unique<OpdsSettingsActivity>(renderer, mappedInputManager, OPDS_STORE.getCount() - 1);
        editor = activity.get();
        activityManager.replaceActivity(std::move(activity));
        break;
      }
      case 1: {
        require(activityManager.isCurrentActivity(editor), "folder editor did not open");
        require(!editor->editServer.useServerFilename, "new server filename toggle must default off");
        editor->selectedIndex = 4;
        editor->handleSelection();
        require(editor->editServer.filenameFormat == OpdsFilenameFormat::TITLE_AUTHOR, "filename choice missing");
        editor->selectedIndex = 5;
        editor->topIndex = 4;
        render();
        capture("server-filename-off");
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        editor->loop();
        mappedInputManager.simulatorClearInputFrame();
        require(editor->editServer.useServerFilename, "button did not enable server filename");
        require(OPDS_STORE.getServer(editor->serverIndex)->useServerFilename, "toggle not saved");
        render();
        capture("server-filename-on");
        editor->onEnter();
        require(editor->editServer.useServerFilename, "toggle lost on reentering settings");
        editor->selectedIndex = 5;
        editor->topIndex = 4;
        render();
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          freeink::ui::Interaction hit;
          bool found = false;
          for (int y = 0; y < renderer.getScreenHeight(); ++y) {
            if (editor->app.hitPublished(renderer.getScreenWidth() / 2, y, 1, hit) && hit.value == 5) {
              found = true;
              break;
            }
          }
          require(found, "filename toggle touch target missing");
          const int x = hit.rect.x + hit.rect.width / 2;
          const int y = hit.rect.y + hit.rect.height / 2;
          mappedInputManager.simulatorInjectTouchDown(x, y);
          editor->loop();
          mappedInputManager.simulatorClearInputFrame();
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          editor->loop();
          mappedInputManager.simulatorClearInputFrame();
        } else
#endif
        {
          editor->handleSelection();
        }
        require(!editor->editServer.useServerFilename, "toggle did not switch off");
        require(editor->editServer.filenameFormat == OpdsFilenameFormat::TITLE_AUTHOR,
                "toggle changed filename format");
        require(!OPDS_STORE.getServer(editor->serverIndex)->useServerFilename, "toggle off not saved");
        const auto originalScale = SETTINGS.uiScale;
        const auto originalOrientation = renderer.getOrientation();
        for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
          renderer.setOrientation(orientation);
          for (uint8_t scale = 0; scale < CrossPointSettings::UI_SCALE_COUNT; ++scale) {
            SETTINGS.uiScale = scale;
            // Fresh metrics/hit targets after changing UI scale.
            editor->onEnter();
            render();
            for (int i = 0; i < 6; ++i) {
              mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
              editor->loop();
              mappedInputManager.simulatorClearInputFrame();
              render();
            }
            require(editor->selectedIndex == 6 && editor->topIndex <= 6 && editor->topIndex + editor->visibleRows > 6,
                    "folder row unreachable with buttons");
            char name[48];
            std::snprintf(name, sizeof(name), "folder-editor-o%d-scale%d", orientation, scale);
            capture(name);
          }
        }
        SETTINGS.uiScale = originalScale;
        renderer.setOrientation(originalOrientation);
        editor->onEnter();
        editor->selectedIndex = 6;
        editor->topIndex = 6;
        render();
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          freeink::ui::Interaction hit;
          bool found = false;
          for (int y = 0; y < renderer.getScreenHeight(); ++y) {
            if (editor->app.hitPublished(renderer.getScreenWidth() / 2, y, 1, hit) && hit.value == 6) {
              found = true;
              break;
            }
          }
          require(found, "folder touch target missing");
          const int x = hit.rect.x + hit.rect.width / 2;
          const int y = hit.rect.y + hit.rect.height / 2;
          mappedInputManager.simulatorInjectTouchDown(x, y);
          editor->loop();
          mappedInputManager.simulatorClearInputFrame();
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          editor->loop();
          mappedInputManager.simulatorClearInputFrame();
        } else
#endif
        {
          editor->handleSelection();
        }
        break;
      }
      case 2:
        require(activityManager.isCurrentActivityNamed("OpdsFolder"), "folder picker missing");
        render();
        capture("folder-options");
        finishWith(OptionSelectionResult{2});
        break;
      case 3:
        require(activityManager.isCurrentActivityNamed("KeyboardEntry"), "custom folder keyboard missing");
        render();
        capture("folder-keyboard");
        finishWith(KeyboardResult{" /Books//Calibre/ "});
        break;
      case 4:
        require(activityManager.isCurrentActivity(editor), "custom path did not return to editor");
        require(editor->editServer.downloadFolder == "/Books/Calibre", "custom path did not normalize/save");
        require(OPDS_STORE.getServer(editor->serverIndex)->downloadFolder == "/Books/Calibre",
                "custom path not stored");
        editor->editDownloadFolder();
        break;
      case 5: {
        ActivityResult result;
        result.isCancelled = true;
        finishWith(std::move(result));
        break;
      }
      case 6:
        require(editor->editServer.downloadFolder == "/Books/Calibre", "cancel changed folder");
        editor->editDownloadFolder();
        break;
      case 7:
        finishWith(OptionSelectionResult{1});
        break;
      case 8:
        require(editor->editServer.downloadFolder == "/", "root override lost");
        render();
        capture("folder-root");
        editor->editDownloadFolder();
        break;
      case 9:
        finishWith(OptionSelectionResult{0});
        break;
      case 10:
        require(editor->editServer.downloadFolder.empty(), "reset to default failed");
        render();
        capture("folder-inherited");
        editor->finish();
        break;
      case 11:
        LOG_INF("SMOKE", "OPDS folder settings: paths, default/root/custom, cancellation and button scrolling passed");
        return false;
      default:
        return false;
    }
    return true;
  }

  static bool tick() {
    if (!std::getenv("CROSSINK_SIMULATOR_SMOKE_OPDS_CATALOG")) return false;
    if (settingsTick()) return true;
    static int phase = 0;
    static OpdsBookBrowserActivity* browser = nullptr;
    switch (phase++) {
      case 0: {
        OpdsServer server;
        server.name = "Catalog smoke";
        server.url = "https://opds.example";
        server.useServerFilename = true;
        auto activity = std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInputManager, std::move(server));
        browser = activity.get();
        activityManager.replaceActivity(std::move(activity));
        break;
      }
      case 1:
        require(activityManager.isCurrentActivity(browser), "browser did not open");
        render();
        outcomes(*browser);
        collision(*browser);
        break;
      case 2:
        closeConfirmation(true);
        break;
      case 3:
        require(activityManager.isCurrentActivity(browser), "cancel did not return to catalog");
        restored(*browser);
        collision(*browser);
        break;
      case 4: {
        RenderLock lock;
        // The callback must use its owned request, even if this row changes.
        browser->entries[browser->selectorIndex].title = "Wrong replacement book";
        browser->entries[browser->selectorIndex].href = "/books/wrong.epub";
      }
        closeConfirmation(false);
        break;
      case 5:
        require(activityManager.isCurrentActivity(browser), "approval did not return to download");
        render();
        {
          RenderLock lock;
          require(browser->state == State::DOWNLOADING && !browser->entries, "approved download retained catalog");
          require(browser->statusMessage == ORIGINAL_TITLE, "overwrite approval changed selected book");
        }
        browser->finishDownload({}, HttpDownloader::ABORTED, "");
        restored(*browser);
        // A catalog that shrinks must clamp both indices before drawing rows.
        browser->downloadSelectorIndex = browser->downloadTopIndex = 50;
        browser->restoreCatalogAfterDownload();
        restored(*browser);
        // Existing locally named files must still prompt before releasing rows.
        std::snprintf(SETTINGS.opdsDownloadFolder, sizeof(SETTINGS.opdsDownloadFolder), "%s", "/books");
        browser->server.filenameFormat = OpdsFilenameFormat::TITLE_AUTHOR;
        browser->server.useServerFilename = false;
        require(Storage.writeFile("/books/The Time Machine - H. G. Wells.epub", "original"), "overwrite fixture");
        browser->requestDownload(browser->entries[2]);
        require(browser->entries != nullptr, "catalog released before initial overwrite approval");
        break;
      case 6:
        closeConfirmation(false);
        break;
      case 7:
        require(activityManager.isCurrentActivity(browser), "local overwrite approval did not start download");
        render();
        require(!browser->entries && browser->statusMessage == "The Time Machine", "local overwrite target lost");
        browser->finishDownload({}, HttpDownloader::ABORTED, "");
        restored(*browser);
        require(Storage.remove("/books/The Time Machine - H. G. Wells.epub"), "remove inherited collision");
        browser->server.downloadFolder = "/books/Calibre";
        require(Storage.mkdir("/books/Calibre"), "custom folder fixture");
        require(Storage.writeFile("/books/Calibre/The Time Machine - H. G. Wells.epub", "original"),
                "custom collision");
        browser->requestDownload(browser->entries[2]);
        require(browser->entries != nullptr, "custom folder ignored for overwrite lookup");
        break;
      case 8:
        closeConfirmation(true);
        break;
      case 9:
        require(activityManager.isCurrentActivity(browser), "custom cancel did not restore browser");
        restored(*browser);
        require(Storage.remove("/books/Calibre/The Time Machine - H. G. Wells.epub"), "remove custom collision");
        browser->server.downloadFolder = "/";
        require(Storage.writeFile("/The Time Machine - H. G. Wells.epub", "original"), "root collision");
        browser->requestDownload(browser->entries[2]);
        require(browser->entries != nullptr, "root override ignored for overwrite lookup");
        break;
      case 10:
        closeConfirmation(true);
        break;
      case 11:
        require(activityManager.isCurrentActivity(browser), "root cancel did not restore browser");
        restored(*browser);
        browser->server.useServerFilename = true;
        start(*browser);
        browser->goHomeAfterCancel = true;
        browser->finishDownload({}, HttpDownloader::ABORTED, "");
        require(!browser->entries, "Home cancellation unnecessarily reloaded catalog");
        break;
      case 12:
        require(activityManager.isCurrentActivityNamed("Home"), "Home cancellation did not exit browser");
        render();
        LOG_INF("SMOKE",
                "Simulator smoke test passed: OPDS folders, catalog release, success/cancel/retry, overwrite and Home");
        std::_Exit(0);
    }
    return true;
  }
};

bool tickOpdsCatalogSmokeTest() { return OpdsCatalogSmokeTest::tick(); }
#endif
