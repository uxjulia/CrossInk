#pragma once
#include <string>
class Epub {
 public:
  enum class XLocationLoadMode { Immediate };
  Epub(const std::string&, const char*) {}
  bool load(bool, bool, XLocationLoadMode, bool) { return true; }
  std::string getTitle() const { return "Book"; }
  std::string getAuthor() const { return "Author"; }
  int getSpineItemsCount() const { return 12; }
  struct Spine {
    int tocIndex = 0;
  };
  Spine getSpineItem(int) const { return {}; }
  int getTocItemsCount() const { return 1; }
  struct Toc {
    std::string title = "Chapter";
  };
  Toc getTocItem(int) const { return {}; }
};
