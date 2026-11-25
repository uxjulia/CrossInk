#include "AtomicFile.h"

namespace atomic_file {
bool recover(const Operations& ops, const char* path, const char* backup) {
  if (!ops.exists(ops.context, backup)) return true;
  if (ops.exists(ops.context, path) && !ops.remove(ops.context, path)) return false;
  return ops.rename(ops.context, backup, path);
}
bool publish(const Operations& ops, const char* path, const char* temporary, const char* backup) {
  const bool previous = ops.exists(ops.context, path);
  if (previous && !ops.rename(ops.context, path, backup)) return false;
  if (!ops.rename(ops.context, temporary, path)) {
    if (previous) recover(ops, path, backup);
    return false;
  }
  if (previous && !ops.remove(ops.context, backup)) {
    recover(ops, path, backup);
    return false;
  }
  return true;
}
}  // namespace atomic_file
