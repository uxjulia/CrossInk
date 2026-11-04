#pragma once

#include <cstddef>

// Exports the newest valid local stats payload to /.crossink-stats-backup/ using a dated or
// incrementing filename. Returns true on success. When outFileName is provided,
// it receives the written filename without the directory prefix.
bool backupGlobalStats(bool manual, char* outFileName = nullptr, size_t outFileNameLen = 0);

// Keeps at least one backup, and up to keep in each daily/manual/clockless set.
// Returns the number removed; negative keep disables pruning.
int pruneBackups(int keep = 7);
