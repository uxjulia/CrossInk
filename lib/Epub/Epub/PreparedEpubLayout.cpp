#include "PreparedEpubLayout.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <ZipFile.h>

#include <array>

#include "Section.h"

namespace prepared_epub_layout {
namespace {

constexpr uint32_t ReceiptVersion = 1;

struct Receipt {
  uint64_t sourceSize;
  uint64_t inventory;
  uint32_t sourceModified;
  uint32_t version;
  std::array<uint32_t, 3> layout;
  uint32_t spineCount;
  uint32_t faces;
  uint32_t faceSignature;
  bool operator==(const Receipt&) const = default;
};
static_assert(sizeof(Receipt) == 48);

void mix(uint64_t& hash, uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; }

// Walk each FAT directory once. Directory entries supply names, sizes and
// timestamps without seeking into chapter files. Missing files or interrupted
// file swaps change the inventory even when a computer edits the cache.
bool inventory(const Epub& epub, int spineCount, const EpubCancellation& cancellation, uint64_t& hash) {
  hash = 14695981039346656037ULL;
  for (int spine = 0; spine < spineCount; spine += Section::ChaptersPerDirectory) {
    if (cancellation.isCancellationRequested()) return false;
    const auto path = Section::layoutDirectory(epub, spine);
    auto dir = Storage.open(path.c_str());
    if (!dir) return false;
    if (!dir.isDirectory()) {
      dir.close();
      return false;
    }
    mix(hash, spine);
    auto entry = dir.openNextFile();
    while (entry) {
      char name[64];
      const auto len = entry.getName(name, sizeof(name));
      const bool readable = len > 0 && len < sizeof(name) - 1 && !entry.isDirectory();
      if (readable) {
        mix(hash, ZipFile::fnvHash64(name, len));
        mix(hash, entry.fileSize64());
        mix(hash, entry.creationTime());
        mix(hash, entry.modificationTime());
      }
      const bool closed = entry.close();
      if (!readable || !closed || cancellation.isCancellationRequested()) {
        dir.close();
        return false;
      }
      entry = dir.openNextFile();
    }
    const bool read = !FsHelpers::directoryIterationFailed(dir);
    const bool closed = dir.close();
    if (!read || !closed) return false;
    delay(1);
  }
  return !cancellation.isCancellationRequested();
}

bool identify(const Epub& epub, const ReaderRenderSpec& spec, int spineCount, const EpubCancellation& cancellation,
              Receipt& receipt) {
  if (spineCount <= 0 || cancellation.isCancellationRequested()) return false;
  auto source = Storage.open(epub.getPath().c_str());
  if (!source) return false;
  receipt.sourceSize = source.fileSize64();
  receipt.sourceModified = source.modificationTime();
  if (!source.close() || !receipt.sourceModified) return false;
  receipt.version = ReceiptVersion;
  receipt.layout = Section::completedLayoutIdentity(epub, spec);
  receipt.spineCount = spineCount;
  return true;
}

}  // namespace

bool matches(const Epub& epub, GfxRenderer& renderer, const ReaderRenderSpec& spec, int spineCount,
             const EpubCancellation& cancellation) {
  auto file = Storage.open(path(epub.getCachePath()).c_str());
  if (!file) return false;
  Receipt saved{};
  const bool read = file.fileSize() == sizeof(saved) && file.read(&saved, sizeof(saved)) == sizeof(saved);
  if (!file.close() || !read || saved.version != ReceiptVersion) return false;
  Receipt current{};
  current.inventory = saved.inventory;
  current.faces = saved.faces;
  current.faceSignature = saved.faceSignature;
  return identify(epub, spec, spineCount, cancellation, current) && saved == current &&
         inventory(epub, spineCount, cancellation, current.inventory) && current.inventory == saved.inventory &&
         renderer.restoreBookFonts(saved.faces, saved.faces, saved.faceSignature);
}

void save(const Epub& epub, GfxRenderer& renderer, const ReaderRenderSpec& spec, int spineCount, uint32_t faces,
          const EpubCancellation& cancellation) {
  Receipt receipt{};
  if (!identify(epub, spec, spineCount, cancellation, receipt) ||
      !inventory(epub, spineCount, cancellation, receipt.inventory))
    return;
  receipt.faces = faces;
  receipt.faceSignature = renderer.bookFontSignature(faces);

  // A receipt is optional and its fixed-size record rejects torn writes.
  // Chapter publication removes the old receipt before changing the layout.
  const auto destination = path(epub.getCachePath());
  HalFile file;
  if (!Storage.openFileForWrite("CHG", destination.c_str(), file)) return;
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&receipt), sizeof(receipt)) == sizeof(receipt);
  const bool closed = file.close();
  if (!written || !closed) {
    LOG_ERR("CHG", "Could not save prepared layout: %s", destination.c_str());
    Storage.remove(destination.c_str());
  }
}

}  // namespace prepared_epub_layout
