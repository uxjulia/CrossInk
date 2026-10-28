#include <FontCatalogIndex.h>
#include <HalStorage.h>
#include <MemoryBudget.h>
#include <SdCardFontRegistry.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <fstream>

static void put(const char* path, const char* text) {
  std::filesystem::create_directories(std::filesystem::path(testRoot + path).parent_path());
  std::ofstream(testRoot + path) << text;
}
int main() {
  char temp[] = "/tmp/crossink-font-index-XXXXXX";
  testRoot = mkdtemp(temp);
  put("/.fonts/Bitmap/Bitmap_10.cpfont", "bitmap10");
  put("/.fonts/Bitmap/Bitmap_14.cpfont", "bitmap14");
  put("/fonts/Bitmap/Bitmap_22.cpfont", "shadowed");
  put("/fonts/Other/Other_12.cpfont", "other");
  put("/.fonts/Vector/Vector-Regular.OTF", "Vector 0");
  put("/.fonts/Vector/Vector-Bold.ttf", "Vector 1");
  // An empty/invalid hidden family must not hide a valid visible install.
  std::filesystem::create_directories(testRoot + "/.fonts/Fallback");
  put("/fonts/Fallback/Fallback_16.cpfont", "fallback");
  // Empty directories must not consume the 128-family limit before a valid
  // visible-root family is considered.
  for (int family = 0; family < SdCardFontRegistry::MAX_SD_FAMILIES; ++family) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/.fonts/Empty%03d", testRoot.c_str(), family);
    std::filesystem::create_directories(path);
  }
  put("/fonts/AfterEmpty/AfterEmpty_18.cpfont", "after-empty");
  SdCardFontRegistry registry;
  assert(registry.loadNames());
  // Discovery releases its 128-entry scratch reservation before returning.
  assert(registry.getFamilies().capacity() == registry.getFamilies().size());
  assert(Storage.exists(fontcatalog::Path));
  assert(!registry.needsRefresh());
  const auto steps = directorySteps;
  registry.clear();
  assert(registry.loadNames());
  assert(directorySteps == steps);
  assert(registry.getFamilies().capacity() == registry.getFamilies().size());
  // A catalog written before CFF support must be rescanned even when the
  // directory names and file sizes have not changed.
  {
    std::fstream index(testRoot + fontcatalog::Path, std::ios::binary | std::ios::in | std::ios::out);
    constexpr uint32_t previousVersion = 2;
    index.seekp(offsetof(fontcatalog::Header, version));
    index.write(reinterpret_cast<const char*>(&previousVersion), sizeof(previousVersion));
  }
  registry.clear();
  assert(registry.loadNames());
  assert(directorySteps > steps);
  {
    std::ifstream index(testRoot + fontcatalog::Path, std::ios::binary);
    fontcatalog::Header header;
    index.read(reinterpret_cast<char*>(&header), sizeof(header));
    assert(index && header.version == fontcatalog::Version);
  }
  for (const auto& family : registry.getFamilies()) assert(family.files.empty());
  const auto* bitmap = registry.findFamily("Bitmap");
  assert(bitmap && bitmap->availableSizes() == std::vector<uint8_t>({10, 14}));
  const auto* fallback = registry.findFamily("Fallback");
  assert(fallback && fallback->availableSizes() == std::vector<uint8_t>({16}));
  assert(fallback->files.front().path == "/fonts/Fallback/Fallback_16.cpfont");
  fallback->releaseDetails();
  const auto* afterEmpty = registry.findFamily("AfterEmpty");
  assert(afterEmpty);
  afterEmpty->releaseDetails();
  bitmap->releaseDetails();
  assert(bitmap->files.empty());
  assert(bitmap->availableSizes() == std::vector<uint8_t>({10, 14}));
  const auto* vector = registry.findFamily("Vector");
  assert(vector && vector->isScalable() && vector->files.size() == 2);
  vector->releaseDetails();
  assert(vector->files.empty());
  for (const auto& family : registry.getFamilies())
    if (family.name != "Bitmap") assert(family.files.empty());

  // External additions/removals are detected at the next metadata boundary.
  put("/.fonts/Bitmap/Bitmap_15.cpfont", "bitmap15");
  assert(registry.loadNames(true));
  assert(registry.findFamily("Bitmap")->availableSizes() == std::vector<uint8_t>({10, 14, 15}));
  Storage.remove("/.fonts/Bitmap/Bitmap_10.cpfont");
  assert(registry.loadNames(true));
  assert(registry.findFamily("Bitmap")->availableSizes() == std::vector<uint8_t>({14, 15}));
  // Explicit invalidation also handles same-length replacements.
  SdCardFontRegistry::invalidateIndex();
  assert(registry.needsRefresh());
  assert(!Storage.exists(fontcatalog::Path));
  assert(registry.loadNames());
  // Truncated summary cannot publish a partial catalog.
  put(fontcatalog::Path, "broken");
  assert(registry.loadNames());
  assert(registry.findFamily("Other"));
  // A broken detail block is rejected and the following lookup boundary repairs it.
  registry.clear();
  assert(registry.loadNames());
  const auto summary = registry.getFamilies().front();
  {
    std::fstream index(testRoot + fontcatalog::Path, std::ios::binary | std::ios::in | std::ios::out);
    index.seekp(summary.indexOffset);
    index.put('\xff');
  }
  assert(!summary.ensureDetails());
  assert(!Storage.exists(fontcatalog::Path));
  assert(registry.loadNames(true));
  assert(registry.findFamily("Bitmap")->availableSizes().size() == 2);
  // Failed inventory scans are recoverable and cannot overwrite the last good index.
  failDirectoryScan = true;
  assert(!registry.loadNames(true) && registry.lastDiscoveryFailed());
  assert(Storage.exists(fontcatalog::Path));
  failDirectoryScan = false;
  assert(registry.loadNames() && !registry.lastDiscoveryFailed());
  registry.clear();
  MemoryBudget::available = 100;
  assert(!registry.loadNames() && registry.lastDiscoveryFailed());
  assert(Storage.exists(fontcatalog::Path));
  MemoryBudget::available = 1024 * 1024;
  assert(registry.loadNames());
  MemoryBudget::available = 100;
  assert(!registry.getFamilies().front().ensureDetails());
  assert(!registry.findFamily(registry.getFamilies().front().name));
  assert(Storage.exists(fontcatalog::Path));
  MemoryBudget::available = 1024 * 1024;
  assert(registry.getFamilies().front().ensureDetails());

  // Reproduce the issue's larger catalog shape and verify a streaming caller
  // can retain names while bounding hydrated paths to one family at a time.
  for (int family = 0; family < 24; ++family) {
    char path[96];
    std::snprintf(path, sizeof(path), "/.fonts/Family%02d/Family%02d_12.cpfont", family, family);
    put(path, "font");
  }
  assert(registry.loadNames(true));
  assert(registry.getFamilyCount() == 29);
  for (const auto& family : registry.getFamilies()) {
    assert(family.ensureDetails());
    assert(!family.files.empty());
    family.releaseDetails();
    assert(family.files.empty());
  }

  // The family cap must be stable regardless of FAT directory order. A
  // lexically early family added after more than 128 valid families survives.
  for (int family = 0; family < 128; ++family) {
    char path[96];
    std::snprintf(path, sizeof(path), "/.fonts/ZFamily%03d/ZFamily%03d_12.cpfont", family, family);
    put(path, "font");
  }
  put("/.fonts/Aardvark/Aardvark_18.cpfont", "aardvark");
  assert(registry.loadNames(true));
  assert(registry.getFamilyCount() == SdCardFontRegistry::MAX_SD_FAMILIES);
  assert(registry.findSummary("Aardvark"));

  // A WebUI mutation removes the cache. Rebuilding a larger catalog under a
  // C3-like budget must publish summaries without retaining every path.
  SdCardFontRegistry::invalidateIndex();
  registry.clear();
  MemoryBudget::available = 64 * 1024;
  assert(registry.loadNames());
  assert(Storage.exists(fontcatalog::Path));
  for (const auto& family : registry.getFamilies()) assert(family.files.empty());
  std::filesystem::remove_all(testRoot);
}
