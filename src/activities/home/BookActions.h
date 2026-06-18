#pragma once

#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"

class GfxRenderer;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
bool hasClearableBookCache(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isEpubCompleted(const std::string& fullPath);
bool toggleEpubCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg);

}  // namespace BookActions
