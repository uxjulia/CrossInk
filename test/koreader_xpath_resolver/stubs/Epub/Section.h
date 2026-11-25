#pragma once

#include <Epub.h>

#include <memory>
#include <optional>

class Section {
 public:
  Section(const std::shared_ptr<Epub>&, int, GfxRenderer&, const char*) {}
  std::optional<int> getCachedPageCount() const { return std::nullopt; }
  std::optional<int> getPageForParagraphIndex(uint16_t) const { return std::nullopt; }
};
