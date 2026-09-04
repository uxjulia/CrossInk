/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossPoint Reader
 */

#pragma once

#include <HalStorage.h>

#include <functional>
#include <memory>
#include <string>

#include "XtcTypes.h"

namespace xtc {

/**
 * XTC File Parser
 *
 * Reads XTC files from SD card and extracts page data.
 * Designed for ESP32-C3's limited RAM (~380KB) using streaming.
 *
 * The source file is kept closed between reads to free heap for rendering.
 * It is reopened on-demand for page table lookups and bitmap data reads.
 */
class XtcParser {
 public:
  XtcParser();
  ~XtcParser();

  // File open/close
  XtcError open(const char* filepath);
  void close();
  bool isOpen() const { return m_isOpen; }

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH

  // Page information
  bool getPageInfo(uint32_t pageIndex, PageInfo& info);

  /**
   * Load page bitmap (raw 1-bit data, skipping XTG header)
   *
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer (caller allocated)
   * @param bufferSize Buffer size
   * @return Number of bytes read on success, 0 on failure
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);

  /**
   * Streaming page load
   * Memory-efficient method that reads page data in chunks.
   *
   * @param pageIndex Page index
   * @param callback Callback function to receive data chunks
   * @param chunkSize Chunk size (default: 1024 bytes)
   * @return Error code
   */
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  size_t getChapterCount();
  size_t getChapters(size_t firstIndex, ChapterInfo* chapters, size_t capacity);
  bool getChapter(size_t index, ChapterInfo& chapter);
  bool getChapterForPage(uint32_t page, ChapterInfo& chapter, size_t* chapterIndex = nullptr);

  // Validation

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  HalFile m_file;
  std::string m_filepath;
  bool m_isOpen;
  XtcHeader m_header;
  std::string m_title;
  std::string m_author;
  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;  // 1 = XTC/XTG (1-bit), 2 = XTCH/XTH (2-bit)
  bool m_hasChapters;
  bool m_chapterInfoLoaded;
  size_t m_chapterCount;
  uint64_t m_chapterOffset;
  // True when every scanned row is a usable chapter, so a logical index maps
  // straight onto its source row. Sparse tables need the forward walk below.
  bool m_chapterTableDense = true;
  bool m_chapterCursorValid = false;
  size_t m_chapterCursorLogical = 0;
  size_t m_chapterCursorSource = 0;
  XtcError m_lastError;
  std::unique_ptr<uint8_t[]> m_streamChunk;
  size_t m_streamChunkSize = 0;

  // Internal helper functions
  XtcError readHeader();
  XtcError readFirstPageInfo();
  XtcError readTitle();
  XtcError readAuthor();
  XtcError readChapterTableInfo();
  bool parseChapterRow(const uint8_t* row, ChapterInfo& chapter, bool& isTerminator) const;
  bool readChapter(size_t index, ChapterInfo& chapter);
  bool readPageTableEntry(uint32_t pageIndex, PageInfo& info);

  // File handle management — reopen on demand, close after use
  bool ensureFileOpen();
  void closeFile();
};

}  // namespace xtc
