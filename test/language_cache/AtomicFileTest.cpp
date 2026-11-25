#include <AtomicFile.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#define CHECK(...)                                                   \
  do {                                                               \
    if (!(__VA_ARGS__)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)
struct Files {
  std::map<std::string, std::string> files{{"settings", "old JSON"}, {"temporary", "new JSON"}};
  std::vector<std::map<std::string, std::string>> boundaries;
  int fail = -1, operations = 0;
  static bool exists(void* ctx, const char* path) { return static_cast<Files*>(ctx)->files.count(path); }
  static bool remove(void* ctx, const char* path) {
    auto& self = *static_cast<Files*>(ctx);
    if (self.operations++ == self.fail) return false;
    if (!self.files.erase(path)) return false;
    self.boundaries.push_back(self.files);
    return true;
  }
  static bool rename(void* ctx, const char* from, const char* to) {
    auto& self = *static_cast<Files*>(ctx);
    if (self.operations++ == self.fail || !self.files.count(from) || self.files.count(to)) return false;
    self.files[to] = self.files.at(from);
    self.files.erase(from);
    self.boundaries.push_back(self.files);
    return true;
  }
  atomic_file::Operations ops() { return {this, exists, remove, rename}; }
};
int main() {
  Files success;
  CHECK(atomic_file::publish(success.ops(), "settings", "temporary", "backup"));
  CHECK(success.files.at("settings") == "new JSON" && !success.files.count("backup"));
  for (const auto& boundary : success.boundaries) {
    Files interrupted;
    interrupted.files = boundary;
    CHECK(atomic_file::recover(interrupted.ops(), "settings", "backup"));
    CHECK(interrupted.files.at("settings") == (boundary.count("backup") ? "old JSON" : "new JSON"));
  }
  for (int failure = 0; failure < 3; ++failure) {
    Files failing;
    failing.fail = failure;
    CHECK(!atomic_file::publish(failing.ops(), "settings", "temporary", "backup"));
    failing.fail = -1;
    CHECK(atomic_file::recover(failing.ops(), "settings", "backup"));
    CHECK(failing.files.at("settings") == "old JSON");
  }
  for (int failure = 0; failure < 2; ++failure) {
    Files recovering;
    recovering.files = {{"settings", "new JSON"}, {"backup", "old JSON"}};
    recovering.fail = failure;
    CHECK(!atomic_file::recover(recovering.ops(), "settings", "backup"));
    CHECK(recovering.files.at("backup") == "old JSON");
    recovering.fail = -1;
    CHECK(atomic_file::recover(recovering.ops(), "settings", "backup"));
    CHECK(recovering.files.at("settings") == "old JSON");
  }
  Files first;
  first.files.erase("settings");
  CHECK(atomic_file::publish(first.ops(), "settings", "temporary", "backup"));
  CHECK(first.files.at("settings") == "new JSON");
  std::puts("Settings publication tests passed: failed renames, interrupted publication, and backup recovery");
}
