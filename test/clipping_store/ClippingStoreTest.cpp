#include <HalStorage.h>
#include <uzlib.h>

#include <cassert>
#include <cstdlib>
#include <new>

#include "ClippingStore.h"

namespace {
bool failArray = false;
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (std::exchange(failArray, false)) return nullptr;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* bytes) noexcept { std::free(bytes); }
void operator delete[](void* bytes, const std::nothrow_t&) noexcept { std::free(bytes); }
void SyncPending::SessionMarker::mark(const std::string&, uint8_t) {}

namespace {
constexpr char BOOK[] = "/book.epub";
ClippingStore& store = ClippingStore::getInstance();
template <class T>
void pod(std::string& out, T value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void string(std::string& out, const std::string& value) {
  pod(out, static_cast<uint32_t>(value.size()));
  out += value;
}
std::string path() {
  return "/.crosspoint/clippings/epub_" + std::to_string(uzlib_crc32(BOOK, sizeof(BOOK) - 1, 0)) + ".bin";
}
std::string bytes(const std::string& name) {
  const auto& v = fake::files.at(name)->bytes;
  return {v.begin(), v.end()};
}
std::string fixture(uint8_t version = 4, uint16_t count = 2) {
  std::string out;
  pod(out, version);
  pod(out, count);
  string(out, "Title");
  string(out, "Author");
  string(out, BOOK);
  for (uint16_t i = 0; i < count; ++i) {
    for (uint16_t x : {i, uint16_t(0), uint16_t(0), uint16_t(1), uint16_t(0), uint16_t(2), uint16_t(3), uint16_t(0)})
      pod(out, x);
    pod(out, uint32_t(0));
    if (version >= 3) pod(out, uint32_t(123));
    if (version >= 4) pod(out, uint16_t(UINT16_MAX));
    out.append(CLIPPING_CHAPTER_TITLE_MAX, '\0');
    if (version == 1)
      pod(out, uint32_t(7));
    else
      pod(out, uint16_t(7));
    out += "passage";
  }
  return out;
}
bool load() { return store.loadForBook(BOOK, "Title", "Author", "epub"); }
auto add() { return store.addClipping(0, 0, 0, 1, 0, 2, 3, "Chapter", 0, "new passage", UINT16_MAX, 123); }
void reset() {
  failArray = false;
  store.unload();
  fake::reset();
}
void cannotMutate(const std::string& original) {
  assert(!store.hasClippings());
  assert(add() == ClippingStore::AddResult::SaveFailed);
  assert(!store.removeClippingAt(0));
  assert(!store.cacheResolvedLayoutRange(0, 0, 0, 1, 123));
  assert(!store.saveToFile());
  store.clearAll();
  store.unload();
  assert(bytes(path()) == original);
}
}  // namespace
int main() {
  // Every readable version still loads and rewrites in CrossInk's v4 format.
  for (uint8_t version = 1; version <= 4; ++version) {
    reset();
    fake::add(path(), fixture(version));
    assert(load());
    assert(store.clippingCount() == 2);
    std::string text;
    assert(store.readClippingText(1, text) && text == "passage");
    assert(add() == ClippingStore::AddResult::Added);
    store.unload();
    assert(load());
    assert(store.clippingCount() == 3);
  }
  // Every truncation boundary, including a complete first record and partial second.
  const std::string good = fixture();
  for (size_t cut = 0; cut < good.size(); ++cut) {
    reset();
    const auto truncated = good.substr(0, cut);
    fake::add(path(), truncated);
    assert(!load());
    cannotMutate(truncated);
  }
  reset();
  std::string huge;
  pod(huge, uint8_t(4));
  pod(huge, uint16_t(1));
  pod(huge, UINT32_MAX);
  fake::add(path(), huge);
  assert(!load());
  cannotMutate(huge);
  reset();
  fake::add(path(), good);
  failArray = true;
  assert(!load());
  cannotMutate(good);
  reset();
  assert(load());
  failArray = true;
  assert(add() == ClippingStore::AddResult::SaveFailed);
  assert(!store.hasClippings());
  assert(!fake::files.contains(path()));
  assert(add() == ClippingStore::AddResult::Added);
  // Allocation failure during a rewrite leaves both disk and index intact.
  const auto saved = bytes(path());
  failArray = true;
  assert(add() == ClippingStore::AddResult::SaveFailed);
  assert(store.clippingCount() == 1 && bytes(path()) == saved);
  store.unload();
  reset();
  fake::add(path(), fixture(4, CLIPPING_MAX_PER_BOOK));
  assert(load());
  assert(add() == ClippingStore::AddResult::LimitReached);
  reset();
  fake::add(path() + ".bak", good);
  assert(load());
  assert(store.clippingCount() == 2);
  assert(fake::files.contains(path()) && !fake::files.contains(path() + ".bak"));
  reset();
  fake::add(path() + ".bak", good);
  fake::failRename = 0;
  assert(!load());
  assert(add() == ClippingStore::AddResult::SaveFailed);
  store.unload();
  assert(bytes(path() + ".bak") == good && !fake::files.contains(path()));
  reset();
  fake::add(path(), good);
  assert(load());
  assert(!store.loadForBook(BOOK, "T", "A", "xtc"));
  cannotMutate(good);
  reset();
  fake::add(path(), good);
  assert(load());
  fake::failWrite = 0;
  assert(!store.removeClippingAt(0));
  assert(store.clippingCount() == 2 && bytes(path()) == good);
  assert(store.saveToFile());
  reset();
}
