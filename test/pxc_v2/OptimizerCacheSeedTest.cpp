#include <HalStorage.h>
#include <Logging.h>

#include <cassert>

#include "lib/Epub/Epub/image/OptimizerCachePublish.h"
#include "lib/Epub/Epub/image/PxcHeaderSink.h"

constexpr size_t kOptimizerPxcExtractionChunkSize = 256;
size_t pxcByteCount(int w, int h) { return 4 + ((w + 3) / 4) * h; }
bool readPxcHeader(FsFile& file, uint16_t& w, uint16_t& h) {
  uint8_t bytes[4];
  if (file.read(bytes, 4) != 4) return false;
  w = bytes[0] | (bytes[1] << 8);
  h = bytes[2] | (bytes[3] << 8);
  return true;
}
namespace OptimizerFormat {
bool dimensions(int w, int h) { return w > 0 && h > 0; }
}  // namespace OptimizerFormat
struct PxcV2Workspace {};
struct Entry {
  int width = 8, height = 4, format = 1;
  size_t bytes = 12;
  std::string pxcHref = "image.pxc";
};
struct PxcV2 : Print {
  PxcV2(PxcV2Workspace&, FsFile&, const Entry&, int, int) { assert(false); }
  size_t write(uint8_t) override { return 0; }
  bool finish() { return false; }
};
struct ZipDirectoryCache {};
struct ZipFile {
  ZipFile(const std::string&, ZipDirectoryCache*) {}
  bool readStoredFileToStream(const std::string&, Print&) { return false; }
};
template <class T>
std::unique_ptr<T> makeUniqueNoThrow() {
  return std::make_unique<T>();
}
struct Epub {
  std::string filepath;
  mutable ZipDirectoryCache zipDirectoryCache;
  mutable std::unique_ptr<PxcV2Workspace> optimizerWorkspace;
  Entry optimizerLastHit;
  std::vector<uint8_t> source = {8, 0, 4, 0, 0x1b, 0xe4, 0x55, 0xaa, 0x33, 0xcc, 0, 255};
  size_t sent = 12;
  bool inflateOk = true;
  mutable int sourceCopies = 0, resized = 0, inflated = 0;
  bool findOptimizerImage(const std::string&) const { return true; }
  bool getItemSize(const std::string&, size_t* size) const {
    *size = optimizerLastHit.bytes;
    return true;
  }
  bool readItemContentsToStream(const std::string&, Print& sink, size_t chunk) const {
    ++inflated;
    assert(chunk == 256);
    for (size_t i = 0; i < sent; ++i)
      if (sink.write(source[i]) != 1) return false;
    return inflateOk;
  }
  bool extractItemToFile(const std::string&, const std::string& path, size_t) const {
    ++sourceCopies;
    Storage.put(path, source);
    return true;
  }
  bool rescalePxcFile(const std::string&, const std::string&, int, int) const {
    ++resized;
    return true;
  }
  bool seedOptimizerImageCache(const std::string&, int, int, const std::string&) const;
};
#include "Seed.inc"
int main() {
  Epub epub;
  Storage.reset();
  assert(epub.seedOptimizerImageCache("image", 8, 4, "/cache"));
  assert(Storage.bytes("/cache") == epub.source);
  assert(epub.inflated == 1 && epub.sourceCopies == 0 && epub.resized == 0);
  assert(!Storage.exists("/cache.optimizer.tmp") && !Storage.exists("/cache.optimizer.source"));
  assert(epub.seedOptimizerImageCache("image", 8, 4, "/cache"));
  assert(epub.inflated == 1);  // Existing valid cache requires no extraction.
  for (int failure = 0; failure < 4; ++failure) {
    Storage.reset();
    Epub broken;
    Storage.put("/cache", {1, 2, 3});  // Old incomplete layout must survive failed publication.
    if (failure == 0) broken.source[0] = 9;
    if (failure == 1) broken.sent = 3;
    if (failure == 2) broken.inflateOk = false;
    if (failure == 3) Storage.failRenameFrom = "/cache.optimizer.tmp";
    assert(!broken.seedOptimizerImageCache("image", 8, 4, "/cache"));
    assert(Storage.bytes("/cache") == std::vector<uint8_t>({1, 2, 3}));
    assert(!Storage.exists("/cache.optimizer.tmp"));
  }
  Storage.reset();
  Epub resize;
  assert(resize.seedOptimizerImageCache("image", 4, 2, "/cache"));
  assert(resize.inflated == 0 && resize.sourceCopies == 1 && resize.resized == 1);
}
