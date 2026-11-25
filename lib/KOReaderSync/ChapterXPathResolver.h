#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve the Nth list item in a spine item to its real XHTML ancestry path.
   *
   * An empty string means parsing failed or the list item index was not found.
   */
  static std::string findXPathForListItem(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t listItemIndex);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   * Counts all visible body text, excluding trailing XML formatting whitespace.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress,
                                          uint8_t minimumPathDepth = 0);

  // minimumPathDepth keeps split-book anchors inside the mapped children,
  // selecting the next child (or the last content character at the end).
  // Resolve a zero-based visible Unicode codepoint offset. This is the stable
  // page position stored in CrossInk's section cache, independent of layout.
  // Text may live directly in body or in any visible element. XML whitespace
  // targets advance to the next non-whitespace codepoint without renumbering
  // text nodes. With no following text, return empty (or the split-book fallback).
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset, uint8_t minimumPathDepth = 0);
};
