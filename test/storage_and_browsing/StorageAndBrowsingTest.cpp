#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
using String = std::string;
constexpr char BACKUP_DIR[] = "/.crossink-stats-backup";
struct BackupName {
  char value[64] = {};
};
struct FsFile {
  bool opened = false;
  bool directory = false;
  std::string name;
  std::vector<std::string> entries;
  size_t cursor = 0;
  explicit operator bool() const { return opened; }
  bool isDirectory() const { return directory; }
  void close() { opened = false; }
  size_t getName(char* buffer, size_t size) const {
    const int written = snprintf(buffer, size, "%s", name.c_str());
    return written >= 0 && static_cast<size_t>(written) < size ? written : 0;
  }
  FsFile openNextFile() {
    if (cursor == entries.size()) return {};
    FsFile file;
    file.opened = true;
    file.name = entries[cursor++];
    return file;
  }
};
struct StorageMock {
  std::map<std::string, int> files;
  std::set<std::string> failedRenames;
  std::string failedRemove;
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return path != failedRemove && files.erase(path) > 0; }
  bool rename(const char* from, const char* to) {
    if (failedRenames.contains(from) || !files.contains(from) || files.contains(to)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  FsFile open(const char* path) const {
    FsFile dir;
    dir.opened = dir.directory = true;
    const std::string prefix = std::string(path) + '/';
    for (const auto& [name, value] : files) {
      (void)value;
      if (name.starts_with(prefix)) dir.entries.push_back(name.substr(prefix.size()));
    }
    return dir;
  }
} Storage;
struct LibraryActivity {
  static constexpr int GRID_PAGE_SIZE = 6;
  int gridCoverWidth = 100, gridCoverHeight = 100;
  int gridPageStart = 0, loadedGridPageStart = -1, nextGridCoverRow = -1;
  bool gridCoverAdded = false;
  int redraws = 0;
  std::vector<bool> covers;
  std::vector<int> attempted;
  bool gridEnabled() const { return true; }
  int rowCount() const { return covers.size(); }
  bool loadGridCover(int row) {
    attempted.push_back(row);
    return covers.at(row);
  }
  void requestUpdate() { ++redraws; }
  void loadGridPageCovers();
};
// Percentage reads must not create an SD cache as a side effect.
int percentWrites = 0;
void saveCachedEpubPercentToCachePath(const std::string&, float) { ++percentWrites; }
float clampProgressPercent(float p) { return std::clamp(p, 0.0f, 100.0f); }
struct RecentBook {
  std::string path = "/book.epub";
};
struct Epub {
  Epub(const std::string&, const char*) {}
  bool load(bool, bool) const { return true; }
  std::string getCachePath() const { return "/cache"; }
  float calculateProgress(int, float page) const { return page / 4; }
};
struct BookMetadataCache {
  explicit BookMetadataCache(const std::string&) {}
  bool load() const { return true; }
  int getSpineCount() const { return 4; }
  size_t getSpineCumulativeSize(int index) const { return (index + 1) * 100; }
};
namespace EpubReaderUtils {
struct Progress {
  bool hasPageCount = true;
  int pageCount = 10, pageNumber = 4, spineIndex = 0;
};
bool loadProgressPath(const std::string&, Progress&, const char*) { return true; }
bool loadProgress(Epub&, Progress&, const char*) { return true; }
}  // namespace EpubReaderUtils
#include "Production.inc"

void putBackup(const std::string& name) { Storage.files[std::string(BACKUP_DIR) + '/' + name] = 1; }
bool hasBackup(const std::string& name) { return Storage.exists((std::string(BACKUP_DIR) + '/' + name).c_str()); }
int main() {
  assert(loadEpubSizeProgressPercentFromCachePath("/cache") == 12.5f);
  assert(loadEpubProgressPercent(RecentBook{}) == 12.5f);
  assert(percentWrites == 0);

  // Replacement, preserve failure, publish failure, failed rollback, and occupied backup.
  for (int failure = 0; failure < 5; ++failure) {
    Storage = {};
    Storage.files = {{"/book", 1}, {"/new", 2}};
    if (failure == 1) Storage.failedRenames.insert("/book");
    if (failure == 2 || failure == 3) Storage.failedRenames.insert("/new");
    if (failure == 3) Storage.failedRenames.insert("/book.davold");
    if (failure == 4) Storage.files["/book.davold"] = 3;
    assert(replaceFile("/new", "/book") == (failure == 0));
    if (failure == 0) {
      assert(Storage.files.at("/book") == 2 && !Storage.exists("/book.davold"));
    } else {
      assert(Storage.files.at("/new") == 2);
      assert(Storage.files.at(failure == 3 ? "/book.davold" : "/book") == 1);
    }
    if (failure == 4) assert(Storage.files.at("/book.davold") == 3);
  }
  Storage = {};
  Storage.files = {{"/book", 1}, {"/new", 2}};
  Storage.failedRemove = "/book.davold";
  assert(replaceFile("/new", "/book"));
  assert(Storage.files.at("/book") == 2 && Storage.files.at("/book.davold") == 1);
  // No retry may throw away a preserved backup.
  Storage.files["/next"] = 3;
  assert(!replaceFile("/next", "/book"));
  assert(Storage.files.at("/book.davold") == 1);
  Storage = {};
  Storage.files["/new"] = 2;
  assert(replaceFile("/new", "/book") && Storage.files.at("/book") == 2);

  Storage = {};
  for (int i = 1; i <= 9; ++i) {
    putBackup("stats_2026-10-0" + std::to_string(i) + ".bin");
    putBackup("stats_2026-10-09_120" + std::to_string(i) + ".bin");
  }
  putBackup("unrelated.bin");
  assert(pruneBackups(7) == 4);
  assert(!hasBackup("stats_2026-10-02.bin") && hasBackup("stats_2026-10-03.bin"));
  assert(!hasBackup("stats_2026-10-09_1202.bin") && hasBackup("stats_2026-10-09_1203.bin"));
  assert(hasBackup("unrelated.bin"));
  assert(pruneBackups(7) == 0);
  Storage = {};
  for (auto name : {"stats_backup_998.bin", "stats_backup_999.bin", "stats_backup_1000.bin", "stats_2026-10-06.bin"})
    putBackup(name);
  assert(pruneBackups(-1) == 0 && Storage.files.size() == 4);
  assert(pruneBackups(0) == 2);
  assert(hasBackup("stats_backup_1000.bin") && hasBackup("stats_2026-10-06.bin"));

  // Losing the clock after seven manual backups must not delete the new backup.
  Storage = {};
  for (int i = 1; i <= 7; ++i) putBackup("stats_2026-10-06_120" + std::to_string(i) + ".bin");
  putBackup("stats_backup_001.bin");
  assert(pruneBackups(7) == 0);
  assert(hasBackup("stats_backup_001.bin"));
  for (int i = 2; i <= 8; ++i) putBackup("stats_backup_00" + std::to_string(i) + ".bin");
  assert(pruneBackups(7) == 1);
  assert(!hasBackup("stats_backup_001.bin") && hasBackup("stats_backup_008.bin"));
  assert(hasBackup("stats_2026-10-06_1201.bin"));

  LibraryActivity library;
  library.covers = {true, false, true, false, false, true, true, true};
  for (int i = 0; i < 5; ++i) {
    library.loadGridPageCovers();
    assert(library.redraws == 0);
  }
  library.loadGridPageCovers();
  assert(library.redraws == 1 && library.attempted.size() == 6);
  library.loadGridPageCovers();
  assert(library.redraws == 1 && library.attempted.size() == 6);
  library.gridPageStart = 6;
  library.nextGridCoverRow = -1;
  library.loadGridPageCovers();
  library.loadGridPageCovers();
  assert(library.redraws == 2 && library.attempted.size() == 8);
  LibraryActivity empty;
  empty.covers = {false, false};
  empty.loadGridPageCovers();
  empty.loadGridPageCovers();
  assert(empty.redraws == 0);
  // A navigation change resets the pending redraw from the abandoned page.
  LibraryActivity changed;
  changed.covers = {true, true, true, true, true, true, false};
  changed.loadGridPageCovers();
  changed.gridPageStart = 6;
  changed.nextGridCoverRow = -1;
  changed.loadGridPageCovers();
  assert(changed.redraws == 0 && changed.loadedGridPageStart == 6);
}
