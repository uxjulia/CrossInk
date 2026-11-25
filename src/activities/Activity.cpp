#include "Activity.h"

#include "ActivityManager.h"
#include "CrossPointState.h"
#include "FilenameFontSystem.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "reader/EpubReaderMenuModel.h"

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
  // Load on the main task before the screen measures or renders translated
  // labels. The service is cache-first and refuses loads during SD handoff.
  filenameFontSystem.ensureLoaded(renderer);
}

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

RequestUpdateResult Activity::requestUpdateAndWait() { return activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finishAfterBackPress() {
  mappedInput.suppressNextBackRelease();
  finish();
}

void Activity::finish() { activityManager.popActivity(); }

bool Activity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (result.bookPath.empty() || result.action == FrontlightPanelAction::None) return false;
  if (result.action != FrontlightPanelAction::SyncProgress &&
      result.action != FrontlightPanelAction::NearbyPositionSync &&
      result.action != FrontlightPanelAction::SendNearbyBook) {
    return false;
  }

  // Home restores its drawer; File Browser captures its list return route
  // in its override. Other screens use the external-book reader flow.
  const bool restoreHomeDrawer = isHomeActivity();
  PendingOverlayResume resume;
  resume.origin = PendingOverlayOrigin::Home;
  resume.overlay = PendingOverlayType::FrontlightDrawer;
  resume.selectedIndex = result.state.selectedAction;
  resume.bookPath = result.bookPath;
  resume.returnHomeAfterReaderFlow = result.action == FrontlightPanelAction::NearbyPositionSync;
  if (result.action == FrontlightPanelAction::SyncProgress) {
    if (restoreHomeDrawer && KOREADER_STORE.hasCredentials()) APP_STATE.setPendingOverlayResume(resume);
    return startGlobalSyncProgress();
  }
  if (result.action == FrontlightPanelAction::NearbyPositionSync) {
    activityManager.goToReaderAndRunMenuAction(result.bookPath,
                                               static_cast<uint8_t>(EpubReaderMenuAction::NEARBY_POSITION_SYNC));
    if (restoreHomeDrawer) APP_STATE.setPendingOverlayResume(std::move(resume));
    return true;
  }
  if (!activityManager.goToNearbyBookSend(result.bookPath, false)) return false;
  if (restoreHomeDrawer) APP_STATE.setPendingOverlayResume(std::move(resume));
  return true;
}
