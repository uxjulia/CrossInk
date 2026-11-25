#include <LanguageCache.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace language_cache;
#define CHECK(...)                                                   \
  do {                                                               \
    if (!(__VA_ARGS__)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

namespace {
std::vector<Key> keys;
std::vector<std::string> englishText;
Key at(size_t i) { return keys[i]; }
const char* english(uint16_t i) { return englishText[i].c_str(); }
void schemaInit(bool changed = false) {
  englishText = changed ? std::vector<std::string>{"Added", "Home", "Changed menu", "%u min"}
                        : std::vector<std::string>{"Home", "Menu", "%u min"};
  keys = changed ? std::vector<Key>{{hash("STR_NEW"), hash("Added"), "STR_NEW", 0, false},
                                    {hash("STR_HOME"), hash("Home"), "STR_HOME", 1, false},
                                    {hash("STR_MENU"), hash("Changed menu"), "STR_MENU", 2, false},
                                    {hash("STR_TIME_FORMAT"), hash("%u min"), "STR_TIME_FORMAT", 3, true}}
                 : std::vector<Key>{{hash("STR_HOME"), hash("Home"), "STR_HOME", 0, false},
                                    {hash("STR_MENU"), hash("Menu"), "STR_MENU", 1, false},
                                    {hash("STR_TIME_FORMAT"), hash("%u min"), "STR_TIME_FORMAT", 2, true}};
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.hash < b.hash; });
}
Schema schema() { return {keys.size(), at, english}; }
struct Source {
  std::string bytes;
  size_t pos = 0;
  int failAt = -1;
  static int read(void* ctx, void* dest, size_t n) {
    auto& s = *static_cast<Source*>(ctx);
    if (s.failAt >= 0 && s.pos >= static_cast<size_t>(s.failAt)) return -1;
    n = std::min(n, s.bytes.size() - s.pos);
    std::memcpy(dest, s.bytes.data() + s.pos, n);
    s.pos += n;
    return static_cast<int>(n);
  }
  static bool rewind(void* ctx) {
    static_cast<Source*>(ctx)->pos = 0;
    return true;
  }
  Input input() {
    pos = 0;
    return {this, read, rewind};
  }
};
struct Storage {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(3 * SLOT_SIZE, 0xff);
  int failMutation = -1;
  int mutations = 0;
  bool torn = false;
  int reads = 0;
  int failRead = -1;
  static bool read(void* ctx, size_t off, void* dest, size_t n) {
    auto& s = *static_cast<Storage*>(ctx);
    if (s.reads++ == s.failRead || off > s.bytes.size() || n > s.bytes.size() - off) return false;
    std::memcpy(dest, s.bytes.data() + off, n);
    return true;
  }
  static bool write(void* ctx, size_t off, const void* data, size_t n) {
    auto& s = *static_cast<Storage*>(ctx);
    if (off > s.bytes.size() || n > s.bytes.size() - off) return false;
    const bool fail = s.mutations++ == s.failMutation;
    if (fail && !s.torn) return false;
    const auto* source = static_cast<const uint8_t*>(data);
    const size_t count = fail ? n / 2 : n;
    for (size_t i = 0; i < count; ++i) {
      CHECK((s.bytes[off + i] & source[i]) == source[i]);
      s.bytes[off + i] &= source[i];
    }
    return !fail;
  }
  static bool erase(void* ctx, size_t off, size_t n) {
    auto& s = *static_cast<Storage*>(ctx);
    CHECK(off >= SLOT_SIZE && off % 4096 == 0 && n % 4096 == 0);
    if (off > s.bytes.size() || n > s.bytes.size() - off) return false;
    const bool fail = s.mutations++ == s.failMutation;
    if (fail && !s.torn) return false;
    std::fill_n(s.bytes.data() + off, fail ? n / 2 : n, 0xff);
    return !fail;
  }
  Flash flash() { return {this, bytes.size(), read, write, erase}; }
  uint8_t* slot(int index) { return bytes.data() + slotOffset(bytes.size(), index); }
};
std::string document(
    std::string body = "STR_HOME: \"Accueil\"\nSTR_MENU: \"Réglages\"\nSTR_TIME_FORMAT: \"%u minutes\"\n",
    const char* code = "FR") {
  return std::string("# Community translation\n_language_code: \"") + code + "\"\n_language_name: \"Français\"\n\n" +
         body;
}
Result put(Storage& storage, const std::string& text, Installed& result, int pinned = -1) {
  Source source{text};
  return install(source.input(), schema(), storage.flash(), pinned, result);
}
void basics() {
  schemaInit();
  Storage storage;
  Installed info;
  CHECK(put(storage, document(), info) == Result::Ok);
  CHECK(info.slot == 0 && info.generation == 1 && std::strcmp(info.metadata.code, "FR") == 0);
  uint16_t offsets[4];
  Installed opened;
  CHECK(open(storage.slot(0), SLOT_SIZE, schema(), opened, offsets));
  CHECK(std::strcmp(reinterpret_cast<char*>(storage.slot(0) + offsets[0]), "Accueil") == 0);
  CHECK(std::strcmp(reinterpret_cast<char*>(storage.slot(0) + offsets[2]), "%u minutes") == 0);
  CHECK(opened.checksum == info.checksum);
  // IDs change, a key is added, and English text changes. Compatible entries survive.
  schemaInit(true);
  CHECK(open(storage.slot(0), SLOT_SIZE, schema(), opened, offsets));
  CHECK(offsets[0] == MISSING && offsets[2] == MISSING);
  CHECK(std::strcmp(reinterpret_cast<char*>(storage.slot(0) + offsets[1]), "Accueil") == 0);
  CHECK(std::strcmp(reinterpret_cast<char*>(storage.slot(0) + offsets[3]), "%u minutes") == 0);
  schemaInit();
  // Applying a replacement never touches the mapped/pinned original.
  const auto old = storage.bytes;
  CHECK(put(storage, document("STR_HOME: \"Bonjour\"\n"), info, 0) == Result::Ok);
  CHECK(info.slot == 1 && info.generation == 2);
  CHECK(std::memcmp(storage.slot(0), old.data() + SLOT_SIZE, SLOT_SIZE) == 0);
  CHECK(open(storage.slot(1), SLOT_SIZE, schema(), opened, offsets));
  CHECK(offsets[1] == MISSING && offsets[2] == MISSING);
  // Simulate the next reboot pinning slot 1, then switch back.
  CHECK(put(storage, document(), info, 1) == Result::Ok);
  CHECK(info.slot == 0 && info.generation == 3);
}
void unchanged() {
  schemaInit();
  Storage original;
  Installed active, result;
  CHECK(put(original, document(), active) == Result::Ok);
  const auto unchanged = [&](const std::string& text) {
    Storage trial = original;
    trial.mutations = 0;
    CHECK(put(trial, text, result, active.slot) == Result::Unchanged);
    CHECK(trial.mutations == 0 && trial.bytes == original.bytes);
    CHECK(result.slot == active.slot && result.generation == active.generation && result.checksum == active.checksum);
    CHECK(std::strcmp(result.metadata.code, active.metadata.code) == 0);
  };
  unchanged(document());
  unchanged(
      "# Another source revision\r\n_language_code : \"fr\" # lowercase normalizes\r\n"
      "_language_name: \"Français\"\n_direction: \"ltr\"\n_keyboard: \"FR\"\n"
      "_template_source_sha256: \"changed source-only metadata\"\n"
      "STR_HOME: \"Accueil\"\nSTR_MENU: \"Réglages\"\nSTR_TIME_FORMAT: \"%u minutes\"\n"
      "STR_UNKNOWN: \"ignored\"\n");
  // Reordering changes the serialized compiled image and uses the bounded linear path.
  Storage reordered = original;
  reordered.mutations = 0;
  CHECK(put(reordered, document("STR_TIME_FORMAT: \"%u minutes\"\nSTR_MENU: \"Réglages\"\nSTR_HOME: \"Accueil\"\n"),
            result, active.slot) == Result::Ok);
  CHECK(reordered.mutations > 0 && result.generation == active.generation + 1);
  // Equality itself needs no rewind callback and must not erase either slot.
  Source same{document()};
  const int before = original.mutations;
  CHECK(install({&same, Source::read}, schema(), original.flash(), active.slot, result) == Result::Unchanged);
  CHECK(original.mutations == before);
  // Changed strings, missing translations, and each runtime metadata field are meaningful.
  for (const auto& text :
       {document("STR_HOME: \"Changed\"\n"), document("STR_HOME: \"Accueil\"\n"), document({}, "DE"),
        std::string("_keyboard: \"HE\"\n") + document(), std::string("_direction: \"rtl\"\n") + document()}) {
    Storage trial = original;
    trial.mutations = 0;
    CHECK(put(trial, text, result, active.slot) == Result::Ok);
    CHECK(trial.mutations > 0 && result.generation == active.generation + 1 && result.slot != active.slot);
    CHECK(std::memcmp(trial.slot(active.slot), original.slot(active.slot), SLOT_SIZE) == 0);
  }
  auto renamed = document();
  renamed.replace(renamed.find("Français"), std::strlen("Français"), "French");
  Storage renamedTrial = original;
  CHECK(put(renamedTrial, renamed, result, active.slot) == Result::Ok);
  // Identical inactive content must not hide a replacement of the active locale.
  Storage inactive = original;
  CHECK(put(inactive, document("STR_HOME: \"Changed\"\n"), result, active.slot) == Result::Ok);
  CHECK(put(inactive, document(), result, result.slot) == Result::Ok && result.generation == 3);
  // Long strings compare across scratch-buffer boundaries, including their tail.
  Storage longSource;
  const auto longText = document("STR_HOME: \"" + std::string(1800, 'x') + "\"\n");
  CHECK(put(longSource, longText, active) == Result::Ok);
  CHECK(put(longSource, longText, result, active.slot) == Result::Unchanged);
  auto changedTail = longText;
  changedTail[changedTail.size() - 3] = 'y';
  CHECK(put(longSource, changedTail, result, active.slot) == Result::Ok);
  // Every flash read in the equality path fails closed before mutation.
  original.reads = 0;
  CHECK(put(original, document(), result, 0) == Result::Unchanged);
  const int readCount = original.reads;
  for (int failure = 0; failure < readCount; ++failure) {
    Storage trial = original;
    trial.reads = trial.mutations = 0;
    trial.failRead = failure;
    CHECK(put(trial, document(), result, 0) == Result::Io);
    CHECK(trial.mutations == 0 && trial.bytes == original.bytes);
  }
  Source broken{document()};
  broken.failAt = 0;
  CHECK(install(broken.input(), schema(), original.flash(), 0, result) == Result::Io);
  CHECK(original.mutations == before);
  for (bool missing : {false, true}) {
    Source changed{document("STR_HOME: \"Changed\"\n")};
    Input input = changed.input();
    input.rewind = missing ? nullptr : +[](void*) { return false; };
    CHECK(install(input, schema(), original.flash(), 0, result) == Result::Io);
    CHECK(original.mutations == before);
  }
}
void parser() {
  schemaInit();
  const auto result = [](const std::string& text) {
    Storage storage;
    Installed i;
    return put(storage, text, i);
  };
  CHECK(result(document("STR_HOME: \"Bonjour\\n\\\"ami\\\"\\\\fin\" # comment\r\n")) == Result::Ok);
  CHECK(result(document("STR_HOME: \"x\"\nSTR_HOME: \"y\"\n")) == Result::Duplicate);
  CHECK(result(document("STR_UNUSED: \"x\"\nSTR_UNUSED: \"y\"\n")) == Result::Duplicate);
  CHECK(result(document("STR_HOME: bare\n")) == Result::Invalid);
  CHECK(result(document("STR_HOME: \"x\\q\"\n")) == Result::Invalid);
  CHECK(result(document("STR_HOME: \"x\" trailing\n")) == Result::Invalid);
  CHECK(result(document("STR_HOME: \"\xc0\xaf\"\n")) == Result::Invalid);
  CHECK(result(document(std::string("STR_HOME: \"a\0b\"\n", 16))) == Result::Invalid);
  CHECK(result(document("# \xff\n")) == Result::Invalid);
  CHECK(result(document("STR_HOME: \"x\"\n_direction: \"rtl\"\n")) == Result::Invalid);
  CHECK(result("STR_HOME: \"x\"\n") == Result::MetadataMissing);
  CHECK(result(document({}, "EN")) == Result::Invalid);
  CHECK(result(document({}, "../FR")) == Result::Invalid);
  CHECK(result(document("STR_HOME: \"" + std::string(2048, 'x') + "\"\n")) == Result::TooLarge);
  CHECK(result(document("STR_NOT_YET_KNOWN: \"anything\"\n")) == Result::Ok);
  CHECK(result(document("STR_HOME: \"\"\n")) == Result::Ok);
  Source metadata{document("STR_HOME: \"x\"", "ZZ-CUSTOM")};
  Metadata m;
  CHECK(inspect(metadata.input(), m) == Result::Ok && std::strcmp(m.code, "ZZ-CUSTOM") == 0 && !m.rtl);
  metadata.bytes = document({}, "AR");
  CHECK(inspect(metadata.input(), m) == Result::Ok && m.rtl && std::strcmp(m.keyboard, "AR") == 0);
  metadata.bytes = "_language_code: \"fa\"\n_language_name: \"Farsi\"\n";
  CHECK(inspect(metadata.input(), m) == Result::Ok && m.rtl && std::strcmp(m.code, "FA") == 0);
  metadata.bytes = "_language_code: \"fa\"\n_language_name: \"Farsi\"\n_direction: \"ltr\"\n";
  CHECK(inspect(metadata.input(), m) == Result::Ok && !m.rtl);
  metadata.bytes = "_language_code: \"foo\"\n_language_name: \"Foo\"\n_direction: \"rtl\"\n_keyboard: \"HE\"\n";
  CHECK(inspect(metadata.input(), m) == Result::Ok && m.rtl && std::strcmp(m.code, "FOO") == 0 &&
        std::strcmp(m.keyboard, "HE") == 0);
}
void validation() {
  schemaInit();
  Metadata metadata;
  const auto check = [&](const std::string& text, const ValidationOptions& options = ValidationOptions{}) {
    Source source{text};
    return validate(source.input(), schema(), metadata, options);
  };
  CHECK(check(document()) == Result::Ok && std::strcmp(metadata.code, "FR") == 0);
  CHECK(check(document("STR_HOME: \"x\"\nSTR_HOME: \"y\"\n")) == Result::Duplicate);
  CHECK(check(document("STR_HOME: bare\n")) == Result::Invalid);
  CHECK(check(document("STR_HOME: \"\xc0\xaf\"\n")) == Result::Invalid);
  CHECK(check(document("STR_HOME: \"ok\"\n_direction: \"rtl\"\n")) == Result::Invalid);
  CHECK(check(document("STR_HOME: \"ok\"\n" + std::string(MAX_SOURCE_SIZE, '#'))) == Result::TooLarge);
  CHECK(check(document("STR_HOME: \"ok\"\nSTR_TIME_FORMAT: \"%n\"\nSTR_UNKNOWN: \"x\"\n")) == Result::Ok);
  CHECK(check("STR_HOME: \"x\"\n") == Result::MetadataMissing);
  ValidationOptions options;
  options.expectedCode = "DE";
  CHECK(check(document(), options) == Result::Invalid && !metadata.code[0]);
  options.expectedCode = "FR";
  options.sourceSha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  options.keysetSha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  CHECK(check(document(), options) == Result::Invalid);
  std::string matched = "_template_source_sha256: \"" + std::string(options.sourceSha256) +
                        "\"\n_template_keyset_sha256: \"" + options.keysetSha256 + "\"\n" + document();
  CHECK(check(matched, options) == Result::Ok);
  matched[matched.find(options.sourceSha256)] = 'b';
  CHECK(check(matched, options) == Result::Invalid);
  options = {};
  size_t calls = 0;
  options.context = &calls;
  options.continueWork = [](void* context) { return ++*static_cast<size_t*>(context) < 3; };
  CHECK(check(document("STR_HOME: \"" + std::string(1000, 'x') + "\"\n"), options) == Result::Cancelled);
  CHECK(calls == 3);
  Source io{document()};
  io.failAt = 0;
  CHECK(validate(io.input(), schema(), metadata) == Result::Io);
  CHECK(validate({}, schema(), metadata) == Result::Invalid);
}
void formats() {
  CHECK(compatibleFormat("%u min", "%u minutes"));
  CHECK(compatibleFormat("%d %.2f%% %zu %lld %s", "%i %.1f%% %zu %lld %s"));
  CHECK(compatibleFormat("%*.*f", "%*.*f"));
  CHECK(!compatibleFormat("%u", "%s"));
  CHECK(!compatibleFormat("%s %u", "%u %s"));
  CHECK(!compatibleFormat("%u", "%n"));
  CHECK(!compatibleFormat("%u", "%1$u"));
  CHECK(!compatibleFormat("%u", "%lu"));
  CHECK(!compatibleFormat("%f", "%*f"));
  CHECK(!compatibleFormat("%u", "%999999999u"));
  CHECK(!compatibleFormat("%u", "%"));
  Storage storage;
  Installed info;
  uint16_t offsets[3];
  CHECK(put(storage, document("STR_TIME_FORMAT: \"%s\"\nSTR_HOME: \"Accueil\"\n"), info) == Result::Ok);
  CHECK(open(storage.slot(info.slot), SLOT_SIZE, schema(), info, offsets));
  CHECK(offsets[2] == MISSING && offsets[0] != MISSING);
  // A later firmware may start formatting a previously literal key without
  // changing its English text. Recheck that contract when building the index.
  CHECK(put(storage, document("STR_HOME: \"%n\"\n"), info) == Result::Ok);
  for (auto& key : keys)
    if (key.id == 0) key.formatted = true;
  CHECK(open(storage.slot(info.slot), SLOT_SIZE, schema(), info, offsets));
  CHECK(offsets[0] == MISSING);
  schemaInit();
}
void capacity() {
  std::vector<std::string> names;
  names.reserve(100);
  englishText.clear();
  englishText.reserve(100);
  keys.clear();
  keys.reserve(100);
  std::string text = document("");
  for (size_t i = 0; i < 100; ++i) {
    names.push_back("STR_LONG_" + std::to_string(i));
    englishText.push_back("English " + std::to_string(i));
    keys.push_back({hash(names.back().c_str()), hash(englishText.back().c_str()), names.back().c_str(),
                    static_cast<uint16_t>(i), false});
    text += names.back() + ": \"" + std::string(1000, 'x') + "\"\n";
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.hash < b.hash; });
  Storage storage;
  Installed info;
  Source source{text};
  Metadata metadata;
  CHECK(validate(source.input(), schema(), metadata) == Result::TooLarge);
  CHECK(storage.mutations == 0);
  CHECK(put(storage, text, info) == Result::TooLarge);
  CHECK(!open(storage.slot(0), SLOT_SIZE, schema(), info));
  schemaInit();
}
void provisioningAndFailures() {
  schemaInit();
  Storage foreign;
  Installed info;
  // Firmware owns this data region regardless of its previous filesystem.
  // Provisioning changes only the chosen cache slot, even with data elsewhere.
  std::fill(foreign.bytes.begin(), foreign.bytes.end(), 0xa5);
  const auto before = foreign.bytes;
  CHECK(put(foreign, document(), info) == Result::Ok && info.slot == 0 && info.generation == 1);
  CHECK(open(foreign.slot(0), SLOT_SIZE, schema(), info));
  CHECK(std::memcmp(foreign.bytes.data(), before.data(), SLOT_SIZE) == 0);
  CHECK(std::memcmp(foreign.slot(1), before.data() + 2 * SLOT_SIZE, SLOT_SIZE) == 0);
  // A first installation interrupted at any flash mutation can be retried.
  for (bool torn : {false, true})
    for (int failure = 0; failure < foreign.mutations; ++failure) {
      Storage trial;
      trial.bytes = before;
      trial.failMutation = failure;
      trial.torn = torn;
      CHECK(put(trial, document(), info) == Result::Io);
      CHECK(std::memcmp(trial.bytes.data(), before.data(), SLOT_SIZE) == 0);
      CHECK(std::memcmp(trial.slot(1), before.data() + 2 * SLOT_SIZE, SLOT_SIZE) == 0);
      trial.failMutation = -1;
      CHECK(put(trial, document(), info) == Result::Ok);
      CHECK(open(trial.slot(info.slot), SLOT_SIZE, schema(), info));
    }
  // Ownership policy does not relax partition capacity/alignment checks.
  for (size_t size : {SLOT_SIZE, 3 * SLOT_SIZE - 1}) {
    Storage invalid;
    invalid.bytes.resize(size);
    CHECK(put(invalid, document(), info) == Result::StorageUnavailable && invalid.mutations == 0);
  }
  Storage original;
  CHECK(put(original, document(), info) == Result::Ok);
  CHECK(put(original, document("STR_HOME: \"Older inactive copy\"\n"), info, 0) == Result::Ok);
  const auto committed = original.bytes;
  Storage success = original;
  success.mutations = 0;
  CHECK(put(success, document("STR_HOME: \"Replacement\"\n"), info, 0) == Result::Ok);
  for (bool torn : {false, true})
    for (int failure = 0; failure < success.mutations; ++failure) {
      Storage trial = original;
      trial.mutations = 0;
      trial.failMutation = failure;
      trial.torn = torn;
      CHECK(put(trial, document("STR_HOME: \"Replacement\"\n"), info, 0) == Result::Io);
      CHECK(std::memcmp(trial.slot(0), committed.data() + SLOT_SIZE, SLOT_SIZE) == 0);
      CHECK(open(trial.slot(0), SLOT_SIZE, schema(), info));
      trial.failMutation = -1;
      CHECK(put(trial, document(), info, 0) == Result::Unchanged);
    }
  // First-ever provisioning can be retried after a torn ownership marker.
  Storage torn;
  torn.failMutation = 1;
  torn.torn = true;
  CHECK(put(torn, document(), info) == Result::Io);
  torn.failMutation = -1;
  CHECK(put(torn, document(), info) == Result::Ok);
  // A failed settings save would keep the old generation. Both remain readable.
  CHECK(open(success.slot(0), SLOT_SIZE, schema(), info) && info.generation == 1);
  CHECK(open(success.slot(1), SLOT_SIZE, schema(), info) && info.generation == 3);
  // Corrupt any committed header/body byte; checksum/structure rejects it.
  for (size_t offset :
       {size_t(16), size_t(20), size_t(28), size_t(32), size_t(36), size_t(40), size_t(72), HEADER_SIZE + 18}) {
    Storage corrupt = original;
    corrupt.slot(0)[offset] ^= 1;
    CHECK(!open(corrupt.slot(0), SLOT_SIZE, schema(), info));
  }
  Source broken{document()};
  broken.failAt = 0;
  CHECK(install(broken.input(), schema(), original.flash(), 0, info) == Result::Io);
}
void batchedInstalls() {
  schemaInit();
  std::vector<uint8_t> staging(SLOT_SIZE);
  const auto batch = [&](Storage& storage, const std::string& text, Installed& info, int pinned = -1) {
    Source source{text};
    return install(source.input(), schema(), storage.flash(), pinned, info, staging.data());
  };
  Storage original;
  Installed info;
  CHECK(batch(original, document(), info) == Result::Ok);
  // Erase, bulk uncommitted image, commit marker: independent of string count.
  CHECK(original.mutations == 3);
  Storage streamed;
  CHECK(put(streamed, document(), info) == Result::Ok);
  CHECK(original.bytes == streamed.bytes);
  CHECK(streamed.mutations > original.mutations);
  const auto before = original.bytes;
  original.mutations = original.reads = 0;
  CHECK(batch(original, document(), info, 0) == Result::Unchanged);
  CHECK(original.mutations == 0 && original.bytes == before);
  const auto replacement = document("STR_HOME: \"Replacement\"\n");
  Storage success = original;
  success.mutations = success.reads = 0;
  CHECK(batch(success, replacement, info, 0) == Result::Ok);
  CHECK(success.mutations == 3);
  CHECK(open(success.slot(0), SLOT_SIZE, schema(), info) && info.generation == 1);
  CHECK(open(success.slot(1), SLOT_SIZE, schema(), info) && info.generation == 2);
  // Fail or tear every persistence operation. A failed marker readback can
  // leave a valid new slot, but the selected old generation always survives.
  for (bool torn : {false, true}) {
    for (int failure = 0; failure < success.mutations; ++failure) {
      Storage trial = original;
      trial.mutations = 0;
      trial.failMutation = failure;
      trial.torn = torn;
      CHECK(batch(trial, replacement, info, 0) == Result::Io);
      CHECK(std::memcmp(trial.slot(0), original.slot(0), SLOT_SIZE) == 0);
      CHECK(open(trial.slot(0), SLOT_SIZE, schema(), info) && info.generation == 1);
      CHECK(!open(trial.slot(1), SLOT_SIZE, schema(), info));
      trial.failMutation = -1;
      CHECK(batch(trial, replacement, info, 0) == Result::Ok);
    }
  }
  for (int failure = 0; failure < success.reads; ++failure) {
    Storage trial = original;
    trial.mutations = trial.reads = 0;
    trial.failRead = failure;
    CHECK(batch(trial, replacement, info, 0) == Result::Io);
    CHECK(std::memcmp(trial.slot(0), original.slot(0), SLOT_SIZE) == 0);
    if (trial.mutations < 3) CHECK(!open(trial.slot(1), SLOT_SIZE, schema(), info));
  }
  // A malformed source is rejected before any persistent mutation.
  Storage invalid = original;
  invalid.mutations = 0;
  CHECK(batch(invalid, "broken", info) != Result::Ok);
  CHECK(invalid.mutations == 0 && invalid.bytes == original.bytes);
  Source broken{document()};
  broken.failAt = 0;
  CHECK(install(broken.input(), schema(), invalid.flash(), -1, info, staging.data()) == Result::Io);
  CHECK(invalid.mutations == 0 && invalid.bytes == original.bytes);
  // Exercise optimized storage verification, including a readback failure
  // before publication. This callback must be honored rather than skipped.
  Storage verified = original;
  auto flash = verified.flash();
  // Reuse the Storage context, with an intentionally failing verifier.
  flash.verify = [](void*, size_t, const void*, size_t) { return false; };
  Source source{replacement};
  verified.mutations = 0;
  CHECK(install(source.input(), schema(), flash, 0, info, staging.data()) == Result::Io);
  CHECK(verified.mutations == 2);
  CHECK(!open(verified.slot(1), SLOT_SIZE, schema(), info));
  CHECK(std::memcmp(verified.slot(0), original.slot(0), SLOT_SIZE) == 0);
}
}  // namespace
void inspectionDiagnostics() {
  Inspector inspector;
  Metadata metadata;
  InspectionDiagnostic diagnostic;
  Source syntax{"_language_code: \"VI\"\n_language_name: unquoted\n"};
  CHECK(inspector.inspect(syntax.input(), metadata, &diagnostic) == Result::Invalid);
  CHECK(diagnostic.line == 2 && diagnostic.offset == syntax.bytes.size());
  CHECK(std::strstr(diagnostic.reason, "YAML syntax"));
  Source bom{std::string("\xef\xbb\xbf") + "_language_code: \"VI\"\n"};
  CHECK(inspector.inspect(bom.input(), metadata, &diagnostic) == Result::Invalid);
  CHECK(diagnostic.line == 1 && std::strstr(diagnostic.reason, "BOM"));
  Source nul{std::string("_language_code: ") + '\0' + "\"VI\"\n"};
  CHECK(inspector.inspect(nul.input(), metadata, &diagnostic) == Result::Invalid);
  CHECK(diagnostic.line == 1 && diagnostic.offset == 17);
  CHECK(std::strstr(diagnostic.reason, "NUL byte"));
  Source missing{"STR_HOME: \"Home\"\n"};
  CHECK(inspector.inspect(missing.input(), metadata, &diagnostic) == Result::MetadataMissing);
  CHECK(std::strstr(diagnostic.reason, "missing _language_code"));
  Source good{"_language_code: \"VI\"\n_language_name: \"Tiếng Việt\"\nSTR_HOME: \"Home\"\n"};
  CHECK(inspector.inspect(good.input(), metadata, &diagnostic) == Result::Ok);
  CHECK(!diagnostic.reason && diagnostic.line == 0 && diagnostic.offset == 0);
  Source broken{good.bytes};
  broken.failAt = 0;
  CHECK(inspector.inspect(broken.input(), metadata, &diagnostic) == Result::Io);
  CHECK(diagnostic.line == 1 && diagnostic.offset == 0);
  CHECK(std::strstr(diagnostic.reason, "source read failed"));
  CHECK(inspector.inspect({}, metadata, &diagnostic) == Result::Invalid);
  CHECK(std::strstr(diagnostic.reason, "missing source reader"));
}

int main() {
  inspectionDiagnostics();
  basics();
  unchanged();
  parser();
  validation();
  formats();
  capacity();
  provisioningAndFailures();
  batchedInstalls();
  std::puts("Language cache tests passed: parsing, formats, compatibility, provisioning and interrupted writes");
}
