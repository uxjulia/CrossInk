#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
inline constexpr size_t CLIPPING_TEXT_MAX = 512;
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 64;

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
  std::string text;
};

struct ClippedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count = 0;
};

class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text);
  bool removeClippingAt(size_t index);
  void saveToFile();
  void clearAll();

  bool hasClippings() const { return !clippings.empty(); }
  bool hasClippingForPage(uint16_t spineIndex, uint16_t page) const;
  const std::vector<Clipping>& getClippings() const { return clippings; }

  static bool hasAnyClippings();
  static bool getAllClippedBooks(std::vector<ClippedBookEntry>& out);
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);

 private:
  static ClippingStore instance;

  std::vector<Clipping> clippings;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readFromFile();
  bool readFromFile(const std::string& path, std::vector<Clipping>& out) const;
  bool writeToFile() const;
};

#define CLIPPINGS ClippingStore::getInstance()
