#pragma once

#include <cstddef>

// Copies /.crosspoint/global_stats.bin to /.crossink-stats-backup/ using a dated or
// incrementing filename. Returns true on success. When outFileName is provided,
// it receives the written filename without the directory prefix.
bool backupGlobalStats(bool manual, char* outFileName = nullptr, size_t outFileNameLen = 0);

// Deletes oldest backup files beyond the keep count. Returns the number removed.
int pruneBackups(int keep = 7);
