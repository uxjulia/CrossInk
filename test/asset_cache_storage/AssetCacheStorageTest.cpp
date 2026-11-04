#include <AssetCacheReadSession.h>
#include <AssetCacheStorage.h>
#include <Memory.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
  char temporary[] = "/tmp/crossink-assets-XXXXXX";
  assert(mkdtemp(temporary));
  const std::filesystem::path root(temporary);
  const auto flashPath = root / "factory.bin";
  const auto sdPath = root / ".crosspoint/asset-cache.bin";
  assert(setenv("CROSSINK_LANGUAGE_FLASH", flashPath.c_str(), 1) == 0);
  assert(setenv("CROSSINK_TEST_SD", temporary, 1) == 0);
  const std::string factory(0x14000, '\x5a');
  std::ofstream(flashPath, std::ios::binary).write(factory.data(), factory.size());
  AssetCacheStorage storage;
#ifdef CROSSINK_ASSET_CACHE_TEST_PSRAM
  assert(storage.begin(false) && storage.usesSd());
  assert(!std::filesystem::exists(sdPath));
  uint8_t data[128];
  assert(storage.read(0, data, sizeof(data)));
  for (auto byte : data) assert(byte == 0xff);
  {
    const auto opens = HalStorage::opens;
    AssetCacheReadSession empty(storage);
    assert(empty && storage.read(0, data, sizeof(data)));
    for (auto byte : data) assert(byte == 0xff);
    assert(HalStorage::opens == opens && HalFile::liveFiles == 0);
  }
  assert(storage.begin(true));
  assert(std::filesystem::file_size(sdPath) == storage.size());
  {
    const auto opens = HalStorage::opens, seeks = HalFile::seeks;
    AssetCacheReadSession reads(storage);
    assert(reads && HalFile::liveFiles == 1);
    // A checksum pass over 128 KiB needs one open and one initial seek,
    // rather than 1024 opens and seeks. The handle keeps its FAT position.
    for (size_t chunk = 0; chunk < 1024; ++chunk) assert(storage.read(4096 + chunk * sizeof(data), data, sizeof(data)));
    assert(HalStorage::opens == opens + 1 && HalFile::seeks == seeks + 1);
    {
      AssetCacheReadSession nested(storage);
      assert(!nested);
    }
    assert(!storage.begin() && !storage.write(0, data, sizeof(data)) && !storage.erase(0, 4096));
    assert(storage.read(0, data, sizeof(data)));  // backward reads still work
    assert(HalFile::seeks == seeks + 2 && HalFile::liveFiles == 1);
    failSnapshotAllocation = true;
    AssetCacheStorage::Mapping failedMap;
    assert(!storage.map(0, sizeof(data), failedMap));
    failSnapshotAllocation = false;
  }
  assert(HalFile::liveFiles == 0);
  HalFile::failOpen = true;
  {
    AssetCacheReadSession failed(storage);
    assert(!failed);
  }
  HalFile::failOpen = false;
  {
    AssetCacheReadSession reads(storage);
    assert(reads);
    HalFile::failRead = true;
    assert(!storage.read(0, data, sizeof(data)));
    HalFile::failRead = false;
  }
  assert(HalFile::liveFiles == 0);
  std::memset(data, 0x42, sizeof(data));
  assert(storage.write(0, data, sizeof(data)));
  {
    const auto opens = HalStorage::opens, seeks = HalFile::seeks;
    // Larger than the verify scratch: one open/seek for the entire readback.
    const std::string expected(4096, '\x42');
    assert(storage.write(4096, expected.data(), expected.size()));
    const auto afterWrite = HalStorage::opens;
    assert(storage.verify(4096, expected.data(), expected.size()));
    assert(HalStorage::opens == afterWrite + 1 && afterWrite == opens + 1);
    assert(HalFile::seeks == seeks + 2 && HalFile::liveFiles == 0);
    assert(!storage.verify(4097, expected.data(), expected.size()));
    HalFile::failRead = true;
    assert(!storage.verify(4096, expected.data(), expected.size()));
    HalFile::failRead = false;
    HalFile::failOpen = true;
    assert(!storage.verify(4096, expected.data(), expected.size()));
    HalFile::failOpen = false;
    HalFile::failClose = true;
    assert(!storage.verify(4096, expected.data(), expected.size()));
    HalFile::failClose = false;
    assert(!storage.verify(storage.size(), expected.data(), expected.size()));
    assert(HalFile::liveFiles == 0);
  }
  AssetCacheStorage::Mapping snapshot;
  assert(storage.map(0, sizeof(data), snapshot));
  assert(snapshot.psram && snapshot.data[0] == 0x42);
  // Another adapter (language vs hyphenation) can use the same image with no
  // persistent open file handles. The active snapshot remains pinned.
  AssetCacheStorage other;
  assert(other.begin());
  assert(other.erase(0, 4096));
  assert(other.read(0, data, sizeof(data)) && data[0] == 0xff);
  assert(snapshot.data[0] == 0x42);
  assert(!other.write(other.size() - 1, data, sizeof(data)));
  assert(!other.erase(1, 4096));
  failSnapshotAllocation = true;
  AssetCacheStorage::Mapping failed;
  assert(!other.map(0, sizeof(data), failed) && !failed.data);
  failSnapshotAllocation = false;
  // Removing SD after activation cannot invalidate pointers.
  std::filesystem::remove(sdPath);
  assert(!other.erase(0, 4096));
  assert(!other.write(0, data, sizeof(data)));
  assert(snapshot.data[0] == 0x42);
  AssetCacheStorage::unmap(snapshot);
  assert(!snapshot.data && !snapshot.psram);
#else
  // A C3-like device must not substitute scarce internal RAM for PSRAM.
  assert(!storage.begin(true) && !storage.usesSd());
  assert(!std::filesystem::exists(sdPath));
#endif
  std::ifstream file(flashPath, std::ios::binary);
  const std::string actual((std::istreambuf_iterator<char>(file)), {});
  file.close();
  assert(actual == factory);
#ifdef CROSSINK_ASSET_CACHE_TEST_PSRAM
  // Stock Picco. Both old banks and the two language slots contain distinct data.
  constexpr size_t slot = 65536, oldBank = (0x80000 - 2 * slot) / 2;
  std::string picco(0x80000, 'A');
  std::fill(picco.begin() + oldBank, picco.begin() + 2 * oldBank, 'B');
  std::fill(picco.begin() + 2 * oldBank, picco.end() - slot, 'L');
  std::fill(picco.end() - slot, picco.end(), 'M');
  std::ofstream(flashPath, std::ios::binary).write(picco.data(), picco.size());
  AssetCacheStorage compact;
  assert(compact.begin() && compact.usesSd() && !std::filesystem::exists(sdPath));
  const size_t bank = (compact.size() - 2 * slot) / 2;
  const auto checkLayout = [&] {
    uint8_t pair[2];
    AssetCacheReadSession session(compact);
    assert(session && compact.read(oldBank - 1, pair, 2) && pair[0] == 'A' && pair[1] == 0xff);
    assert(compact.read(bank, pair, 1) && pair[0] == 'B');
    assert(compact.read(2 * bank - 1, pair, 2) && pair[0] == 0xff && pair[1] == 'L');
    assert(compact.read(compact.size() - slot, pair, 1) && pair[0] == 'M');
  };
  checkLayout();
  HalFile::failClose = true;
  assert(!compact.begin(true) && !std::filesystem::exists(sdPath));
  HalFile::failClose = false;
  assert(compact.begin(true));
  checkLayout();
  const std::string german(206259, 'g');
  assert(german.size() > oldBank && compact.write(bank, german.data(), german.size()));
  AssetCacheStorage reboot;
  assert(reboot.begin() && reboot.verify(bank, german.data(), german.size()));
  std::ifstream preserved(flashPath, std::ios::binary);
  assert(std::string(std::istreambuf_iterator<char>(preserved), {}) == picco);
#endif
  std::filesystem::remove_all(root);
}
