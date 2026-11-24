#pragma once

#include <cstdint>
#include <string>

#include "DirectoryPosition.h"

// Incremental depth-first walk. No book list and no open SD handles between calls.
class FolderBookIterator {
 public:
  enum class Result { Entry, Book, Done, Error };
  enum class TraversalLimit { Fail, Skip };
  explicit FolderBookIterator(std::string root, TraversalLimit limit = TraversalLimit::Fail);
  Result next(std::string& bookPath);

 private:
  static constexpr unsigned MAX_DEPTH = 16;
  static constexpr size_t MAX_PATH = 1024;
  const TraversalLimit limit;
  std::string directory;
  const uint32_t pathRevision;
  DirectoryPosition positions[MAX_DEPTH]{};
  unsigned depth = 0;
  bool done = false;
  // Lives in the activity-owned iterator, not the main task's stack.
  char name[256]{};
};
