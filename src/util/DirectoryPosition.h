#pragma once

#include <HalStorage.h>

// The hardware HAL saves the current FAT cluster as well as the byte offset.
// The simulator's POSIX directory adapter uses an entry ordinal.
#ifdef SIMULATOR
using DirectoryPosition = size_t;
inline DirectoryPosition directoryPosition(const HalFile& directory) { return directory.position(); }
inline bool seekDirectoryPosition(HalFile& directory, const DirectoryPosition position) {
  return !position || directory.seekSet(position);
}
#else
using DirectoryPosition = HalFile::DirectoryPosition;
inline DirectoryPosition directoryPosition(const HalFile& directory) { return directory.directoryPosition(); }
inline bool seekDirectoryPosition(HalFile& directory, const DirectoryPosition& position) {
  return !position.offset || directory.seekDirectoryPosition(position);
}
#endif
