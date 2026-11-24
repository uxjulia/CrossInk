#include "FolderBookIterator.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>

FolderBookIterator::FolderBookIterator(std::string root)
    : directory(std::move(root)), pathRevision(library::libraryPathRevision()) {
  while (directory.size() > 1 && directory.back() == '/') directory.pop_back();
}

FolderBookIterator::Result FolderBookIterator::next(std::string& bookPath) {
  if (done) return Result::Done;
  if (pathRevision != library::libraryPathRevision()) {
    LOG_ERR("ReadingSync", "The library changed during the folder scan");
    return Result::Error;
  }
  auto dir = Storage.open(directory.c_str());
  if (!dir || !dir.isDirectory() || !seekDirectoryPosition(dir, positions[depth])) {
    if (dir) dir.close();
    LOG_ERR("ReadingSync", "Cannot enumerate folder: %s", directory.c_str());
    return Result::Error;
  }
  auto entry = dir.openNextFile();
  if (!entry) {
    const bool failed = FsHelpers::directoryIterationFailed(dir);
    dir.close();
    if (failed) return Result::Error;
    if (depth == 0) {
      done = true;
      return Result::Done;
    }
    --depth;
    const auto slash = directory.find_last_of('/');
    directory.resize(slash == 0 ? 1 : slash);
    return Result::Entry;
  }
  positions[depth] = directoryPosition(dir);
  name[0] = '\0';
  const size_t nameLength = entry.getName(name, sizeof(name));
  const bool folder = entry.isDirectory();
  entry.close();
  dir.close();
  if (nameLength >= sizeof(name) - 1) {
    LOG_ERR("ReadingSync", "Folder entry name exceeds upload limit");
    return Result::Error;
  }
  if (name[0] == '.' || !FsHelpers::isSafePathComponent(std::string_view(name))) return Result::Entry;
  const auto path = directory + (directory == "/" ? "" : "/") + name;
  if (path.size() > MAX_PATH || (folder && depth + 1 >= MAX_DEPTH)) {
    LOG_ERR("ReadingSync", "Folder traversal limit reached: %s", path.c_str());
    return Result::Error;
  }
  if (folder) {
    directory = path;
    positions[++depth] = {};
    return Result::Entry;
  }
  // XTC books carry reading stats too; callers sync their stats without a position.
  if (!FsHelpers::hasEpubExtension(path) && !FsHelpers::hasXtcExtension(path)) return Result::Entry;
  bookPath = path;
  return Result::Book;
}
