#include "BookStatsActivity.h"

#include <I18n.h>

#include "BookStatsView.h"
#include "EpubReaderMenuModel.h"
#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "activities/network/StatsUploadActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "network/ReadingSyncUpload.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncPending.h"
#include "util/InputReleaseGuard.h"

namespace {

void drawPageIndicators(const GfxRenderer& renderer, const int currentPage, const int totalPages) {
  if (totalPages <= 1) {
    return;
  }

  constexpr int kDotSize = 8;
  constexpr int kDotSpacing = 6;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int totalDotWidth = totalPages * kDotSize + (totalPages - 1) * kDotSpacing;
  const int dotsStartX = (renderer.getScreenWidth() - totalDotWidth) / 2;
  const int dotY = renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer) - metrics.verticalSpacing - 4;
  for (int pageIndex = 0; pageIndex < totalPages; ++pageIndex) {
    const int dotX = dotsStartX + pageIndex * (kDotSize + kDotSpacing);
    if (pageIndex == currentPage) {
      renderer.fillRect(dotX, dotY, kDotSize, kDotSize, true);
    } else {
      renderer.drawRect(dotX, dotY, kDotSize, kDotSize, true);
    }
  }
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      savedCompleted(stats.isCompleted),
      globalStats(globalStats),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const GlobalReadingStats& allDevicesStats, const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      savedCompleted(stats.isCompleted),
      globalStats(globalStats),
      allDevicesStats(allDevicesStats),
      showAllDevicesStats(true),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

void BookStatsActivity::refreshAllDevicesStats() {
  if (showAllDevicesStats) {
    allDevicesStats = GlobalReadingStats::loadAggregated(globalStats);
  }
}

void BookStatsActivity::loadDailySummary() {
  dailySummary = {};
  if (!halClock.isAvailable()) {
    return;
  }
  ReadingStatsDateTime now;
  if (!getCurrentLocalDailyReadingDateTime(now)) {
    LOG_DBG("BookStats", "No local date for daily reading time");
    return;
  }

  dailySummary = DailyReadingStats::summarize(readingStatsDayIndex(now.date));
}

void BookStatsActivity::saveStats() {
  if (!didChangeStats || !hasEditableBook()) {
    return;
  }

  if (!stats.save(bookCachePath)) {
    LOG_ERR("BookStats", "Could not save book stats");
    return;
  }
  globalStats.save();
  if (!syncBookPath.empty()) {
    SyncPending::mark(syncBookPath, SyncPending::STATS_BIT);
    // The date editor can mark the book finished (or not) too.
    if (stats.isCompleted != savedCompleted) BookSyncFile::onFinishedChanged(syncBookPath, stats.isCompleted);
  }
  savedCompleted = stats.isCompleted;
  refreshAllDevicesStats();
  didChangeStats = false;
}

void BookStatsActivity::beginDateEditing() {
  dateEditStatsSnapshot = stats;
  dateEditGlobalStatsSnapshot = globalStats;
  didChangeStatsBeforeDateEdit = didChangeStats;
  dateEditSnapshotValid = true;
  page = Page::EditDates;
  requestUpdate();
}

void BookStatsActivity::finishDateEditing(const bool saveChanges) {
  if (saveChanges) {
    saveStats();
  } else if (dateEditSnapshotValid) {
    stats = dateEditStatsSnapshot;
    globalStats = dateEditGlobalStatsSnapshot;
    didChangeStats = didChangeStatsBeforeDateEdit;
    setResult(ReadingStatsResult{didChangeStats});
  }

  dateEditSnapshotValid = false;
  page = Page::PerBook;
  requestUpdate();
}

void BookStatsActivity::cycleEditField() { selectedEditField = (selectedEditField + 1) % 6; }

ReadingStatsDate BookStatsActivity::defaultDateForField(const bool finishedField) const {
  if (finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }
  if (!finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (!finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return now.date;
  }
  return {2000, 1, 1};
}

void BookStatsActivity::applyCompletedState(const bool completed) {
  if (stats.isCompleted == completed) {
    return;
  }

  stats.isCompleted = completed;
  if (completed) {
    globalStats.completedBooks++;
    if (!stats.finishedDateManual && !stats.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
}

void BookStatsActivity::normalizeEditedDates(const bool editedFinishedField) {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid()) {
    return;
  }
  if (compareReadingStatsDate(stats.finishedDate, stats.startDate) >= 0) {
    return;
  }

  if (editedFinishedField) {
    stats.startDate = stats.finishedDate;
  } else {
    stats.finishedDate = stats.startDate;
  }
}

void BookStatsActivity::clearEditedDate(const bool finishedField) {
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  date.clear();

  if (finishedField) {
    stats.finishedDateManual = false;
    applyCompletedState(false);
  } else {
    stats.startDateManual = false;
  }

  didChangeStats = true;
  setResult(ReadingStatsResult{true});
  requestUpdate();
}

bool BookStatsActivity::shouldClearDateOnAdjust(const ReadingStatsDate& date, const bool finishedField,
                                                const int fieldIndex, const int delta) const {
  if (!date.isValid()) {
    return false;
  }

  switch (fieldIndex) {
    case 0:
      return (date.month == 1 && delta < 0) || (date.month == 12 && delta > 0);
    case 1: {
      const uint8_t monthDays = daysInMonth(date.year, date.month);
      return (date.day == 1 && delta < 0) || (date.day == monthDays && delta > 0);
    }
    case 2:
      return (date.year == 2000 && delta < 0) || (date.year == 2099 && delta > 0);
    default:
      return false;
  }
}

void BookStatsActivity::adjustSelectedDateField(const int delta) {
  const bool finishedField = selectedEditField >= 3;
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  const int fieldIndex = selectedEditField % 3;

  if (shouldClearDateOnAdjust(date, finishedField, fieldIndex, delta)) {
    clearEditedDate(finishedField);
    return;
  }

  if (!date.isValid()) {
    date = defaultDateForField(finishedField);
  }

  switch (fieldIndex) {
    case 0: {
      int month = static_cast<int>(date.month) + delta;
      while (month < 1) {
        month += 12;
      }
      while (month > 12) {
        month -= 12;
      }
      date.month = static_cast<uint8_t>(month);
      break;
    }
    case 1: {
      const int monthDays = daysInMonth(date.year, date.month);
      int day = static_cast<int>(date.day) + delta;
      while (day < 1) {
        day += monthDays;
      }
      while (day > monthDays) {
        day -= monthDays;
      }
      date.day = static_cast<uint8_t>(day);
      break;
    }
    case 2: {
      int year = static_cast<int>(date.year) + delta;
      if (year < 2000) {
        year = 2099;
      } else if (year > 2099) {
        year = 2000;
      }
      date.year = static_cast<uint16_t>(year);
      break;
    }
  }

  const uint8_t monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) {
    date.day = monthDays;
  }

  if (finishedField) {
    stats.finishedDateManual = true;
    applyCompletedState(true);
  } else {
    stats.startDateManual = true;
  }
  normalizeEditedDates(finishedField);

  didChangeStats = true;
  setResult(ReadingStatsResult{true});
  requestUpdate();
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreInitialPowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  if (bookCachePath.empty()) page = Page::ThisDevice;
  loadDailySummary();
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void BookStatsActivity::onExit() {
  saveStats();
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void BookStatsActivity::exitStatsActivity() {
  if (returnToHomeOnExit) {
    onGoHome();
    return;
  }

  finish();
}

bool BookStatsActivity::showNextStatsPage() {
  if (page == Page::PerBook) {
    page = Page::ThisDevice;
    requestUpdate();
    return true;
  }

  if (page == Page::ThisDevice && showAllDevicesStats) {
    page = Page::AllDevices;
    requestUpdate();
    return true;
  }

  return false;
}

bool BookStatsActivity::showPreviousStatsPage() {
  if (page == Page::AllDevices) {
    page = Page::ThisDevice;
    requestUpdate();
    return true;
  }

  if (page == Page::ThisDevice) {
    page = Page::PerBook;
    requestUpdate();
    return true;
  }

  return false;
}

bool BookStatsActivity::selectEditFieldFromTouchTarget(const int touchTarget) {
  if (touchTarget < BookStatsTouchTarget::DateFieldBase ||
      touchTarget >= BookStatsTouchTarget::DateFieldBase + BookStatsTouchTarget::DateFieldCount) {
    return false;
  }

  const int newEditField = touchTarget - BookStatsTouchTarget::DateFieldBase;
  if (selectedEditField != newEditField) {
    selectedEditField = newEditField;
    requestUpdate();
  }
  return true;
}

bool BookStatsActivity::canSyncReadingStats() const { return ReadingSyncUpload::canSyncReadingStats(); }

void BookStatsActivity::startSyncReadingStats() {
  // Ask here so Cancel returns to these stats instead of an already-closed reader.
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_SYNC_READING_STATS),
                                                         tr(STR_SYNC_READING_STATS_CONFIRM), true);
  if (!confirm) {
    LOG_ERR("BookStats", "Cannot allocate Sync Reading Stats confirmation");
    return;
  }
  startActivityForResult(std::move(confirm), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    auto sync = makeUniqueNoThrow<StatsUploadActivity>(renderer, mappedInput, std::string{},
                                                       StatsUploadActivity::Scope::ReadingStats, true);
    if (!sync) {
      LOG_ERR("BookStats", "Cannot allocate Sync Reading Stats activity");
      requestUpdate();
      return;
    }
    // Only a reader underneath is a screen to come back to; Home is the default anyway.
    if (activityManager.isReaderActivity())
      sync->setReturnToBook(activityManager.getCurrentBookPath(),
                            static_cast<uint8_t>(EpubReaderMenuAction::READING_STATS));
    saveStats();
    // Close normally first: the parent (often the reader) imports these stats
    // edits in its result handler before the sync replaces the stack.
    activityManager.replaceAfterReturn(std::move(sync));
    finish();
  });
}

void BookStatsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power,
                                               ignoreInitialPowerRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
    if (page == Page::EditDates) {
      finishDateEditing(true);
    } else {
      exitStatsActivity();
    }
    return;
  }
  int syncTarget = -1;
  const bool syncPage = usesNoRtcSingleScreenLayout() || page == Page::ThisDevice;
  if (syncPage && canSyncReadingStats() && mappedInput.wasItemTapped(syncTarget) &&
      syncTarget == BookStatsTouchTarget::SyncAll) {
    startSyncReadingStats();
    return;
  }

  if (usesNoRtcSingleScreenLayout()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mappedInput.suppressNextBackRelease();
      exitStatsActivity();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      mappedInput.suppressNextConfirmRelease();
      if (canSyncReadingStats()) {
        startSyncReadingStats();
        return;
      }
      exitStatsActivity();
      return;
    }
    return;
  }

  const bool upOrLeftPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                               mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool downOrRightPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                                  mappedInput.wasPressed(MappedInputManager::Button::Right);

  if (page == Page::EditDates) {
    int touchedTarget = -1;
    if (mappedInput.wasItemTouchedDown(touchedTarget) && selectEditFieldFromTouchTarget(touchedTarget)) {
      return;
    }
    int tappedTarget = -1;
    if (mappedInput.wasItemTapped(tappedTarget)) {
      if (selectEditFieldFromTouchTarget(tappedTarget)) {
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustUp) {
        adjustSelectedDateField(1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustDown) {
        adjustSelectedDateField(-1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateSave) {
        finishDateEditing(true);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateCancel) {
        finishDateEditing(false);
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishDateEditing(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      cycleEditField();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustSelectedDateField(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustSelectedDateField(1);
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    exitStatsActivity();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    if (page == Page::PerBook && hasEditableBook()) {
      beginDateEditing();
      return;
    }
    if (page == Page::ThisDevice && canSyncReadingStats()) {
      startSyncReadingStats();
      return;
    }
    exitStatsActivity();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && showNextStatsPage()) {
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    if (!showPreviousStatsPage()) {
      exitStatsActivity();
    }
    return;
  }

  if (page == Page::PerBook) {
    int touchedTarget = -1;
    if (hasEditableBook() && mappedInput.wasItemTapped(touchedTarget) &&
        touchedTarget == BookStatsTouchTarget::StartedDaysStat) {
      beginDateEditing();
      return;
    }
    if (hasEditableBook() && upOrLeftPressed) {
      beginDateEditing();
      return;
    }
    if (downOrRightPressed) {
      showNextStatsPage();
      return;
    }
    return;
  }

  if (upOrLeftPressed) {
    showPreviousStatsPage();
    return;
  }

  if (page == Page::ThisDevice && showAllDevicesStats && downOrRightPressed) {
    showNextStatsPage();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  if (usesNoRtcSingleScreenLayout()) {
    renderNoRtcCombinedStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                                 estimatedTimeLeftSeconds, globalStats,
                                 showAllDevicesStats ? &allDevicesStats : nullptr, true, canSyncReadingStats());
    renderer.displayBuffer();
    return;
  }

  switch (page) {
    case Page::PerBook:
      renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                             estimatedTimeLeftSeconds, true, hasEditableBook(), true);
      break;
    case Page::ThisDevice:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_THIS_DEVICE_SCREEN), globalStats, true,
                            showAllDevicesStats, &dailySummary, canSyncReadingStats());
      break;
    case Page::AllDevices:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_ALL_DEVICES_SCREEN), allDevicesStats, true, false);
      break;
    case Page::EditDates:
      renderEditBookDatesPage(renderer, &mappedInput, bookTitle, stats, selectedEditField, true);
      break;
  }
  if (page != Page::EditDates) {
    drawPageIndicators(renderer, static_cast<int>(page), showAllDevicesStats ? 3 : 2);
  }
  renderer.displayBuffer();
}
