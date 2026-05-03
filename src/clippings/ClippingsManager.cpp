#include "ClippingsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include "util/StringUtils.h"

namespace {

std::string clippingPathForBook(const std::string& bookTitle, const std::string& author) {
  const std::string safeTitle = StringUtils::sanitizeFilename(bookTitle.empty() ? "book" : bookTitle, 80);
  const std::string safeAuthor = StringUtils::sanitizeFilename(author, 40);

  std::string filename = safeTitle;
  if (!safeAuthor.empty() && safeAuthor != "book") {
    filename += " - ";
    filename += safeAuthor;
  }
  filename += ".txt";

  return std::string(ClippingsManager::CLIPPINGS_DIR) + "/" + filename;
}

bool isMeaningfulChapterTitle(const std::string& chapterTitle, const std::string& bookTitle) {
  if (chapterTitle.empty()) {
    return false;
  }
  return chapterTitle != bookTitle;
}

}
bool ClippingsManager::saveClipping(const std::string& bookTitle, const std::string& author,
                                    const std::string& chapterTitle, int pageNumber, const std::string& selectedText) {
  if (!Storage.ensureDirectoryExists(CLIPPINGS_DIR)) {
    LOG_ERR("CLIP", "Failed to ensure %s exists", CLIPPINGS_DIR);
    return false;
  }

  const std::string clippingPath = clippingPathForBook(bookTitle, author);
  const bool fileAlreadyExists = Storage.exists(clippingPath.c_str());
  HalFile file = Storage.open(clippingPath.c_str(), O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("CLIP", "Failed to open %s for append", clippingPath.c_str());
    return false;
  }

  std::string fileHeader;
  if (!fileAlreadyExists) {
    fileHeader = bookTitle.empty() ? "book" : bookTitle;
    fileHeader += "\n";
    if (!author.empty()) {
      fileHeader += author;
      fileHeader += "\n";
    }
    fileHeader += "\n---\n\n";
  }

  std::string location;
  if (isMeaningfulChapterTitle(chapterTitle, bookTitle)) {
    location = chapterTitle + " - Page " + std::to_string(pageNumber);
  } else {
    location = "Page " + std::to_string(pageNumber);
  }
  location += "\n\n";

  static constexpr size_t MAX_TEXT = 2000;
  const size_t textLen = selectedText.size() < MAX_TEXT ? selectedText.size() : MAX_TEXT;

  static constexpr char separator[] = "\n\n---\n\n";
  static constexpr size_t separatorLen = sizeof(separator) - 1;
  const size_t totalLen = fileHeader.size() + location.size() + textLen + separatorLen;

  auto* buf = static_cast<char*>(malloc(totalLen));
  if (!buf) {
    LOG_ERR("CLIP", "malloc failed: %zu bytes", totalLen);
    file.close();
    return false;
  }

  size_t pos = 0;
  memcpy(buf + pos, fileHeader.c_str(), fileHeader.size());
  pos += fileHeader.size();
  memcpy(buf + pos, location.c_str(), location.size());
  pos += location.size();
  memcpy(buf + pos, selectedText.c_str(), textLen);
  pos += textLen;
  memcpy(buf + pos, separator, separatorLen);

  const bool ok = file.write(buf, totalLen) == totalLen;
  free(buf);
  file.flush();
  file.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to write clipping to %s (SD full or removed?)", clippingPath.c_str());
    return false;
  }

  LOG_DBG("CLIP", "Saved clipping to %s (%zu chars)", clippingPath.c_str(), textLen);
  return true;
}
