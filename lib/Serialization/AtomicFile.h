#pragma once

namespace atomic_file {
struct Operations {
  void* context;
  bool (*exists)(void*, const char*);
  bool (*remove)(void*, const char*);
  bool (*rename)(void*, const char*, const char*);
};
// A remaining backup means publication did not finish. Restore it before
// reading or starting another write, including when the primary is missing.
bool recover(const Operations& ops, const char* path, const char* backup);
// The caller has already recovered, written, synced, and closed the temporary file.
bool publish(const Operations& ops, const char* path, const char* temporary, const char* backup);
}  // namespace atomic_file
