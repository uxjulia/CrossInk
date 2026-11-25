#!/usr/bin/env python3
"""Exercise production settings-result and stats-transition methods on the host.

Like test_clipping_page_matcher.py, compile actual method bodies with small
fixtures so activity lifecycle transitions can run without hardware or a UI.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


FIXTURES = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
struct RenderLock {
  template<class T> explicit RenderLock(T&){}
  static bool heldByCurrentTask(){return true;}
};
#include "activities/ActivityResult.h"

struct ResultActivity {
  bool ttfRenderingChanged = false;
  bool finished = false;
  ActivityResult result;
  void setResult(ActivityResult value) { result = std::move(value); }
  void finish() { finished = true; }
};
struct SettingsActivity : ResultActivity { void finishToParent(); };

struct Date {
  int value = 0;
  bool isValid() const { return value != 0; }
};
struct ReadingStatsDateTime { Date date{1}; };
struct DeferredReadingStatsInterval { ReadingStatsDateTime end; uint32_t seconds=0; uint8_t utcOffset=255; };
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& result) {
  result.date.value = 2;
  return true;
}
bool getCurrentLocalDailyReadingDateTime(ReadingStatsDateTime& result) {
  return getCurrentLocalReadingStatsDateTime(result);
}
#define LOG_DBG(...) ((void)0)
int dailyLogErrors = 0;
void LOG_ERR(const char*, const char*) { ++dailyLogErrors; }
// Observe the reader's daily-session calls; daily bucketing and persistence
// are covered by DailyReadingStatsTest.
struct DailySession {
  uint32_t recordedSeconds = 0, lastSessionSeconds = 0;
  int starts = 0, resets = 0, accepts = 0;
  bool acceptSucceeds = true;
  void reset() { ++resets; }
  void start(const ReadingStatsDateTime&, uint8_t offset) {
    assert(offset == 48);
    ++starts;
  }
  bool accept(const ReadingStatsDateTime& end, uint8_t offset, uint32_t seconds, uint32_t sessionSeconds, bool = true) {
    assert(end.date.value == 2 && offset == 48);
    ++accepts;
    lastSessionSeconds = sessionSeconds;
    if (!acceptSucceeds) return false;
    recordedSeconds += seconds;
    return true;
  }
};
unsigned long now = 10000;
unsigned long millis() { return now; }
struct Settings {
  bool enabled = true;
  uint8_t clockUtcOffsetQ = 48;
  bool shouldTrackReadingStats() const { return enabled; }
  uint32_t getReadingIdleTimeThresholdSeconds() const { return 300; }
} SETTINGS;
struct Stats {
  uint32_t totalReadingSeconds = 100, sessionCount = 1, totalSessions = 1;
  uint32_t spanSeconds = 0, savedSeconds = 0, completedBooks = 0, estimatedTimeLeftSeconds = 0;
  int saves = 0;
  bool saveFails = false, startDateManual = false, finishedDateManual = false, isCompleted = false;
  Date startDate, finishedDate;
  Stats* disk = nullptr;  // Where save() publishes, standing in for the SD file.
  static Stats diskBook, diskGlobal;
  static uint32_t resetRevision;
  static uint32_t localResetRevision() { return resetRevision; }
  static Stats loadFrom(Stats& file) {
    Stats copy = file;
    copy.disk = &file;
    return copy;
  }
  static Stats load(const std::string&) { return loadFrom(diskBook); }
  static Stats load() { return loadFrom(diskGlobal); }
  void recordReadingSpan(const ReadingStatsDateTime& start, uint32_t seconds) {
    assert(start.date.value == (spanSeconds == 0 ? 1 : 2));
    spanSeconds += seconds;
  }
  bool save(const std::string& = "") {
    ++saves;
    if (saveFails) return false;
    savedSeconds = totalReadingSeconds;
    if (disk) {
      Stats copy = *this;
      copy.disk = nullptr;
      *disk = copy;
    }
    return true;
  }
};
Stats Stats::diskBook, Stats::diskGlobal;
uint32_t Stats::resetRevision = 0;
using BookReadingStats = Stats;
using GlobalReadingStats = Stats;
// Date bucketing is exercised by SessionDates; this fixture observes reader wiring.
struct ReadingSessionDates {
  uint32_t seconds = 111;
  Date first{1};
  void reset() { seconds = 0; first = {}; }
  void start(const ReadingStatsDateTime&, uint8_t) {}
  void accept(const ReadingStatsDateTime& end, uint8_t, uint32_t value) {
    if (!first.isValid()) first = end.date;
    seconds += value;
  }
  void apply(BookReadingStats& book, GlobalReadingStats& global) const {
    book.spanSeconds += seconds;
    global.spanSeconds += seconds;
  }
  const Date& firstReadingDate() const { return first; }
};
struct ReadingSessionJournalStub {
  void start(const ReadingStatsDateTime&, uint8_t) {}
  bool accept(const char*, const ReadingStatsDateTime&, uint8_t, uint32_t, bool = true) { return true; }
  bool flush() { return true; }
  uint32_t revision() const { return 0; }
};
bool saveReadingSessionCheckpoint(const BookReadingStats&, const GlobalReadingStats&, uint32_t,
                                  const ReadingSessionDates*, const std::string&) { return true; }
struct Renderer {} renderer;
struct FontSystem {
  uint32_t generation = 0;
  int reloads = 0;
  uint32_t scalableRenderOptionsGeneration() const { return generation; }
  bool reloadActiveScalableFamily(Renderer&, const char*) {
    ++reloads;
    ++generation;
    return true;
  }
} sdFontSystem;
struct Activity {
  int exits = 0;
  void onExit() { ++exits; }
};
struct TtfRenderOptionsActivity : Activity, ResultActivity {
  Renderer& renderer = ::renderer;
  std::string family_ = "Serif";
  bool changed_ = false, reloadHandled_ = false;
  void onExit();
  void finishWithResult();
};
struct Book {
  bool chapters = true;
  bool hasChapters() const { return chapters; }
  uint32_t getChapterCount() const { return chapters ? 3 : 0; }
  uint32_t getPageCount() const { return 6; }
  std::string getCachePath() const { return "/book"; }
  std::string getPath() const { return "/book.epub"; }
} book;

struct Input {
  void suppressNextConfirmRelease() {}
  void suppressNextPowerRelease() {}
  void suppressNextPowerConfirmRelease() {}
};
struct XtcReaderChapterSelectionActivity {
  XtcReaderChapterSelectionActivity(Renderer&, Input&, Book*, uint32_t) {}
};
bool failChapterAllocation = false;
template <typename T, typename... Args> std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (failChapterAllocation) return {};
  return std::make_unique<T>(std::forward<Args>(args)...);
}
constexpr int STR_NO_CHAPTERS = 0;
const char* tr(int) { return "No chapters"; }
void drawToast(Renderer&, const char*) {}
void delay(unsigned long duration) { now += duration; }
struct SyncPending { static constexpr uint8_t STATS_BIT = 1; };
struct PendingSync { void mark(const std::string&, uint8_t) {} };
struct ReaderState {
  PendingSync pendingSync;
  Book* epub = &book;
  Book* xtc = &book;
  bool section = true, activeFootnotePreview = false;
  bool bookStatsEnabled = true, statsTrackingActive = true;
  bool pendingStatsCommit = false, paceDirty = false;
  ReadingSessionDates readingSessionDates;
  DeferredReadingStatsInterval deferredReadingStats;
  ReadingSessionJournalStub readingSessionJournal;
  uint32_t statsCheckpointRevision = 0;
  unsigned long pageShownAtMs = 1000;
  uint32_t sessionReadingSeconds = 111;
  DailySession dailyReadingSession;
  Stats stats, globalStats;
  uint32_t globalStatsResetRevisionAtPanelOpen = 0;
  int updates = 0;
  ReaderState() {
    stats.disk = &Stats::diskBook;
    globalStats.disk = &Stats::diskGlobal;
  }
  void armReadingPaceWarmup(const char*) {}
  void requestUpdate() { ++updates; }
};
struct EpubReaderActivity : ReaderState {
  uint32_t ttfRenderGenerationAtPanelOpen = 0;
  bool pendingTtfRenderRelayout = false;
  void syncStatsTrackingState();
  void commitReadingStatsSession();
  void finalizeReadingStatsOnExit();
  void onFrontlightPanelOpened();
  void onFrontlightPanelClosed();
  bool currentPageReadingSecondsForStats(uint32_t&, const char*) const;
  void recordCurrentPageReadingTime(const char*, bool=false);
  void flushDeferredReadingStats(bool = true);
  void startDailyReadingInterval();
  void clearPendingManualPageTurns() {}
  void pauseReadingPaceTimer(const char*) {}
  bool saveProgressBeforeRestart() { return true; }
  void recoverStoredPaceFromSession(const char*) {}
  void refreshCachedTimeLeftEstimate() {}
  void resumeReadingPaceTimer(const char*) {}
};
struct XtcReaderActivity : ReaderState {
  void syncStatsTrackingState();
  void commitReadingStats();
  void finalizeReadingStatsOnExit();
  void applyBookStatsEditsFromDisk();
  void onFrontlightPanelClosed();
  bool currentPageReadingSecondsForStats(uint32_t&, const char*) const;
  void recordCurrentPageReadingTime(const char*, bool=false);
  void flushDeferredReadingStats(bool = true);
  void startDailyReadingInterval();
  void resumeReadingStatsTimer(const char*);
  void pauseReadingStatsTimer(const char*);
  void openChapterSelection();
  uint32_t currentPage = 0;
  Renderer& renderer = ::renderer;
  Input mappedInput;
  int chapterStarts = 0;
  template <typename Callback> void startActivityForResult(
      std::unique_ptr<XtcReaderChapterSelectionActivity>, Callback) {
    assert(pageShownAtMs == 0);
    ++chapterStarts;
  }
};
'''

CASES = r'''
void checkXtcChapterFailureStats() {
  SETTINGS.enabled = true;
  for (bool fromMenu : {false, true}) {
    for (bool unavailable : {false, true}) {
      now = 10000;
      XtcReaderActivity reader;
      if (fromMenu) reader.pauseReadingStatsTimer("reader_menu");
      book.chapters = !unavailable;
      failChapterAllocation = !unavailable;
      reader.openChapterSelection();
      assert(reader.chapterStarts == 0);
      assert(reader.sessionReadingSeconds == 120);
      assert(reader.dailyReadingSession.recordedSeconds == 9);
      assert(reader.dailyReadingSession.accepts == 1);
      assert(reader.pageShownAtMs == now);
      assert(reader.updates == 1);
      now += 3000;
      reader.pauseReadingStatsTimer("exit");
      assert(reader.sessionReadingSeconds == 123);
      assert(reader.dailyReadingSession.recordedSeconds == 12);
    }
  }
  book.chapters = true;
  failChapterAllocation = false;
  now = 10000;
  XtcReaderActivity reader;
  reader.openChapterSelection();
  assert(reader.chapterStarts == 1 && reader.sessionReadingSeconds == 120);
  assert(reader.dailyReadingSession.recordedSeconds == 9);
}

template <typename Reader> void checkStats(bool globalToggle) {
  SETTINGS.enabled = true;
  Reader reader;
  if (globalToggle) SETTINGS.enabled = false;
  else reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  assert(!reader.statsTrackingActive);
  assert(reader.sessionReadingSeconds == 0);
  // 111 seconds on previous pages plus 9 seconds on the current page.
  assert(reader.stats.totalReadingSeconds == 220);
  assert(reader.globalStats.totalReadingSeconds == 220);
  assert(reader.stats.savedSeconds == 220 && reader.globalStats.savedSeconds == 220);
  assert(reader.stats.sessionCount == 2 && reader.globalStats.totalSessions == 2);
  assert(reader.stats.spanSeconds == 120 && reader.globalStats.spanSeconds == 120);
  assert(reader.stats.startDate.value == 1);
  assert(reader.dailyReadingSession.recordedSeconds == 9 && reader.dailyReadingSession.accepts == 1);
  assert(reader.dailyReadingSession.lastSessionSeconds == 120);
  assert(reader.dailyReadingSession.resets == 1 && reader.dailyReadingSession.starts == 1);
  reader.syncStatsTrackingState();
  assert(reader.stats.totalReadingSeconds == 220);
  assert(reader.dailyReadingSession.accepts == 1);
  SETTINGS.enabled = true;
  reader.bookStatsEnabled = true;
  reader.syncStatsTrackingState();
  assert(reader.statsTrackingActive && reader.sessionReadingSeconds == 0);
  assert(reader.dailyReadingSession.resets == 2 && reader.dailyReadingSession.starts == 2);
  now += 60000;
  reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  assert(reader.stats.totalReadingSeconds == 280);
  assert(reader.globalStats.totalReadingSeconds == 280);
  assert(reader.stats.sessionCount == 3 && reader.globalStats.totalSessions == 3);
  assert(reader.stats.spanSeconds == 180 && reader.globalStats.spanSeconds == 180);
  assert(reader.stats.startDate.value == 1);
  assert(reader.dailyReadingSession.recordedSeconds == 69 && reader.dailyReadingSession.accepts == 2);
  assert(reader.dailyReadingSession.lastSessionSeconds == 60);
  now = 10000;
}

template <typename Reader> void checkThresholds() {
  SETTINGS.enabled = true;
  for (uint32_t seconds : {9, 10, 59, 60}) {
    Reader reader;
    reader.sessionReadingSeconds = seconds;
    reader.pageShownAtMs = 0;  // Menu already paused the page timer.
    reader.bookStatsEnabled = false;
    reader.syncStatsTrackingState();
    assert(reader.stats.totalReadingSeconds == 100 + (seconds >= 10 ? seconds : 0));
    assert(reader.globalStats.totalReadingSeconds == reader.stats.totalReadingSeconds);
    assert(reader.stats.sessionCount == (seconds >= 60 ? 2 : 1));
    assert(reader.globalStats.totalSessions == reader.stats.sessionCount);
  }
}

template <typename Reader> void checkIdleAndFailure() {
  SETTINGS.enabled = true;
  Reader reader;
  reader.pageShownAtMs = 1000;
  now = 400000;  // Idle page is excluded; already recorded reading survives.
  reader.stats.saveFails = true;
  reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  assert(reader.stats.totalReadingSeconds == 211);
  assert(reader.globalStats.totalReadingSeconds == 211);
  assert(reader.pendingStatsCommit);
  assert(reader.sessionReadingSeconds == 0);
  assert(reader.dailyReadingSession.accepts == 0);  // Idle time does not reach daily stats.
  reader.syncStatsTrackingState();
  assert(reader.stats.totalReadingSeconds == 211);
  now = 10000;
}

template <typename Reader> void checkDailyRecordFailure() {
  SETTINGS.enabled = true;
  Reader reader;
  reader.dailyReadingSession.acceptSucceeds = false;
  const int errors = dailyLogErrors;
  reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  assert(dailyLogErrors == errors + 1);
  assert(reader.stats.totalReadingSeconds == 220 && reader.globalStats.totalReadingSeconds == 220);
  assert(reader.dailyReadingSession.accepts == 1 && reader.dailyReadingSession.recordedSeconds == 0);
  reader.syncStatsTrackingState();
  assert(dailyLogErrors == errors + 1 && reader.dailyReadingSession.accepts == 1);
}

void resetDisk() {
  Stats::diskBook = Stats{};
  Stats::diskGlobal = Stats{};
}

// Exit after a mid-session toggle must not commit the session a second time.
template <typename Reader> void checkExitAfterToggle() {
  SETTINGS.enabled = true;
  resetDisk();
  Reader reader;
  reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  reader.finalizeReadingStatsOnExit();
  assert(reader.stats.totalReadingSeconds == 220 && reader.globalStats.totalReadingSeconds == 220);
  assert(reader.stats.sessionCount == 2 && Stats::diskBook.totalReadingSeconds == 220);

  Reader plain;  // Exit with tracking on commits exactly once.
  plain.finalizeReadingStatsOnExit();
  assert(plain.stats.totalReadingSeconds == 220 && plain.sessionReadingSeconds == 0);
  assert(Stats::diskBook.totalReadingSeconds == 220 && Stats::diskGlobal.totalReadingSeconds == 220);
}

// The drawer stats screen saves a preview that already includes the pending
// session. Closing the drawer must import edits only and keep live counters
// unless drawer Settings reset global stats, then still commit the session once.
template <typename Reader> void checkPanelReload(bool globalReset) {
  SETTINGS.enabled = true;
  resetDisk();
  Reader reader;
  reader.sessionReadingSeconds = 120;
  reader.pageShownAtMs = 0;  // Opening the drawer paused the page timer.
  reader.globalStats.totalReadingSeconds = 500;  // Unsaved live counter.
  reader.globalStatsResetRevisionAtPanelOpen = Stats::localResetRevision();
  Stats::diskBook = reader.stats;
  Stats::diskBook.totalReadingSeconds = 220;  // Preview copy saved by BookStatsActivity.
  Stats::diskBook.startDate.value = 5;
  Stats::diskBook.isCompleted = true;
  Stats::diskGlobal.totalReadingSeconds = 400;  // Older on-disk copy.
  Stats::diskGlobal.completedBooks = 4;
  if (globalReset) {
    Stats::diskGlobal = Stats{};
    Stats::diskGlobal.totalReadingSeconds = 0;
    Stats::resetRevision++;
  }
  reader.onFrontlightPanelClosed();
  assert(reader.stats.totalReadingSeconds == 100);
  assert(reader.stats.startDate.value == 5 && reader.stats.isCompleted);
  assert(reader.globalStats.totalReadingSeconds == (globalReset ? 0 : 500));
  assert(reader.globalStats.completedBooks == (globalReset ? 0 : 4));
  reader.finalizeReadingStatsOnExit();
  assert(Stats::diskBook.totalReadingSeconds == 220 && Stats::diskBook.startDate.value == 5);
  assert(Stats::diskGlobal.totalReadingSeconds == (globalReset ? 120 : 620));
}

// A failed toggle-off save must not lose the committed session when the drawer
// later closes.
template <typename Reader> void checkFailedSaveSurvivesReload(bool globalFailure) {
  SETTINGS.enabled = true;
  resetDisk();
  Reader reader;
  reader.stats.saveFails = !globalFailure;
  reader.globalStats.saveFails = globalFailure;
  reader.bookStatsEnabled = false;
  reader.syncStatsTrackingState();
  assert(reader.pendingStatsCommit && reader.stats.totalReadingSeconds == 220);
  reader.globalStatsResetRevisionAtPanelOpen = Stats::localResetRevision();
  reader.onFrontlightPanelClosed();
  assert(reader.stats.totalReadingSeconds == 220 && reader.globalStats.totalReadingSeconds == 220);
  reader.stats.saveFails = false;
  reader.globalStats.saveFails = false;
  reader.finalizeReadingStatsOnExit();
  assert(Stats::diskBook.totalReadingSeconds == 220 && Stats::diskGlobal.totalReadingSeconds == 220);
}

void checkXtcStatsScreenReturn() {
  SETTINGS.enabled = true;
  resetDisk();
  XtcReaderActivity reader;
  reader.sessionReadingSeconds = 120;
  reader.pageShownAtMs = 0;
  Stats::diskBook = reader.stats;
  Stats::diskBook.totalReadingSeconds = 220;  // Preview copy with the session.
  Stats::diskBook.startDate.value = 7;
  Stats::diskGlobal.completedBooks = 3;
  Stats::diskGlobal.totalReadingSeconds = 999;
  reader.applyBookStatsEditsFromDisk();
  assert(reader.stats.totalReadingSeconds == 100 && reader.stats.startDate.value == 7);
  assert(reader.globalStats.completedBooks == 3 && reader.globalStats.totalReadingSeconds == 100);
  reader.finalizeReadingStatsOnExit();
  assert(Stats::diskBook.totalReadingSeconds == 220);
}

void checkTtfOptionsExit() {
  for (bool changed : {false, true}) {
    for (bool unwound : {false, true}) {
      sdFontSystem.reloads = 0;
      TtfRenderOptionsActivity options;
      options.changed_ = changed;
      if (!unwound) {
        options.finishWithResult();
        assert(std::get<TtfRenderOptionsResult>(options.result.data).activeFamilyChanged == changed);
      }
      options.onExit();  // A Home/Reader unwind reaches only onExit().
      assert(options.exits == 1 && sdFontSystem.reloads == (changed ? 1 : 0));
    }
  }
}

void checkTtfReloadDetection() {
  EpubReaderActivity reader;
  sdFontSystem.generation = 7;
  reader.onFrontlightPanelOpened();
  reader.onFrontlightPanelClosed();
  assert(!reader.pendingTtfRenderRelayout);
  reader.onFrontlightPanelOpened();
  sdFontSystem.generation++;  // Reloaded while the drawer was open, result lost.
  reader.onFrontlightPanelClosed();
  assert(reader.pendingTtfRenderRelayout);
}

// The panel-open snapshot must track earlier resets so a later drawer session
// without a reset keeps live counters.
void checkEpubResetSnapshot() {
  resetDisk();
  Stats::resetRevision = 3;  // A reset happened earlier this boot.
  EpubReaderActivity reader;
  reader.globalStats.totalReadingSeconds = 500;
  Stats::diskGlobal.totalReadingSeconds = 400;
  reader.onFrontlightPanelOpened();
  reader.onFrontlightPanelClosed();
  assert(reader.globalStats.totalReadingSeconds == 500);
  reader.onFrontlightPanelOpened();
  Stats::resetRevision++;
  reader.onFrontlightPanelClosed();
  assert(reader.globalStats.totalReadingSeconds == 400);
}

int main(int argc, char**) {
  if (argc > 1) {
    for (bool changed : {false, true}) {
      SettingsActivity settings;
      settings.ttfRenderingChanged = changed;
      settings.finishToParent();
      auto result = std::get<TtfRenderOptionsResult>(settings.result.data);
      assert(settings.finished && result.activeFamilyChanged == changed);
    }
    checkTtfOptionsExit();
    checkTtfReloadDetection();
  } else {
    for (bool global : {false, true}) {
      checkStats<EpubReaderActivity>(global);
      checkStats<XtcReaderActivity>(global);
    }
    checkThresholds<EpubReaderActivity>();
    checkThresholds<XtcReaderActivity>();
    checkIdleAndFailure<EpubReaderActivity>();
    checkIdleAndFailure<XtcReaderActivity>();
    checkDailyRecordFailure<EpubReaderActivity>();
    checkDailyRecordFailure<XtcReaderActivity>();
    checkExitAfterToggle<EpubReaderActivity>();
    checkExitAfterToggle<XtcReaderActivity>();
    for (bool reset : {false, true}) {
      checkPanelReload<EpubReaderActivity>(reset);
      checkPanelReload<XtcReaderActivity>(reset);
    }
    for (bool globalFailure : {false, true}) {
      checkFailedSaveSurvivesReload<EpubReaderActivity>(globalFailure);
      checkFailedSaveSurvivesReload<XtcReaderActivity>(globalFailure);
    }
    checkXtcStatsScreenReturn();
    checkXtcChapterFailureStats();
    checkEpubResetSnapshot();
  }
}
'''


def main():
    methods = method("src/activities/settings/SettingsActivity.cpp",
                     "void SettingsActivity::finishToParent()")
    ttf = "src/activities/settings/TtfRenderOptionsActivity.cpp"
    methods += method(ttf, "void TtfRenderOptionsActivity::onExit()")
    methods += method(ttf, "void TtfRenderOptionsActivity::finishWithResult()")
    session = "src/activities/reader/ReadingSessionStats.cpp"
    methods += method(session, "void commitReadingSession(")
    methods += method(session, "void importBookStatsEdits(")
    methods += method(session, "void refreshGlobalStatsAfterOverlay(")
    methods += method("src/activities/reader/ReadingStatsUtils.cpp", "bool readingStatsIntervalSeconds(")
    for reader in ("EpubReaderActivity", "XtcReaderActivity"):
        path = f"src/activities/reader/{reader}.cpp"
        for name, result in (("syncStatsTrackingState", "void"),
                             ("currentPageReadingSecondsForStats", "bool"),
                             ("recordCurrentPageReadingTime", "void"),
                             ("flushDeferredReadingStats", "void"),
                             ("startDailyReadingInterval", "void"),
                             ("finalizeReadingStatsOnExit", "void"),
                             ("onFrontlightPanelClosed", "void")):
            methods += method(path, f"{result} {reader}::{name}(")
        if reader == "XtcReaderActivity":
            for name in ("openChapterSelection", "pauseReadingStatsTimer", "resumeReadingStatsTimer"):
                methods += method(path, f"void {reader}::{name}(")
            methods += method(path, f"void {reader}::commitReadingStats()")
            methods += method(path, f"void {reader}::applyBookStatsEditsFromDisk()")
        else:
            methods += method(path, "void EpubReaderActivity::commitReadingStatsSession()")
            methods += method(path, "void EpubReaderActivity::onFrontlightPanelOpened()")
    with tempfile.TemporaryDirectory(prefix="crossink-reader-transitions-") as directory:
        source = Path(directory) / "transitions.cpp"
        binary = Path(directory) / "transitions"
        source.write_text(FIXTURES + methods + CASES)
        subprocess.run(["c++", "-std=c++20", "-I" + str(ROOT / "src"),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([str(binary), "fonts"], check=True)
    print("PASS: font results and reload detection; stats toggles, daily intervals, exit ordering, drawer reloads, and failed saves")


if __name__ == "__main__":
    main()
