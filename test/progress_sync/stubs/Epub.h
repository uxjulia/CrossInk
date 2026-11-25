#pragma once
#include <string>
class Epub {
 public:
  struct SyncMetadata {
    std::string isbn, asin, series, seriesIndex;
  };
  bool loadSyncMetadata(SyncMetadata&) { return false; }
};
