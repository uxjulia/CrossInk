#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"
#include "PendingOverlayResume.h"

class GfxRenderer;
class MappedInputManager;
class Activity;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
// EPUB: reboot into progress sync for this book. XTC: send its stats in place.
void syncProgress(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& fullPath,
                  PendingOverlayResume returnResume);
bool hasClearableBookCache(const std::string& path);
bool canSendNearby(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
// Read-only, log-quiet variant for list rows: never migrates cache folders or
// stats files, and skips books that have no cache folder.
bool isBookCompletedForList(const std::string& fullPath);
bool isBookStatsTrackingEnabled(const std::string& fullPath);
bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg, const char* detail = nullptr);
enum class TouchscreenToggleSource { Button, FrontlightDrawer };
// Only drawer toggles need the Picco recovery hint and longer reading time.
void showTouchscreenToast(const GfxRenderer& renderer, bool readerContext, TouchscreenToggleSource source);

}  // namespace BookActions
