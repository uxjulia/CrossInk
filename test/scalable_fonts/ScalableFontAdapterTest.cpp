#include <HalScalableFont.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using FileMode = HalScalableFont::FileMode;

static size_t tableCount(const char* path) {
  std::ifstream file(path, std::ios::binary);
  uint8_t header[6];
  file.read(reinterpret_cast<char*>(header), sizeof(header));
  assert(file.gcount() == sizeof(header));
  return (size_t(header[4]) << 8) | header[5];
}

static void checkRuntimeAdvances(HalScalableFont& adapter, const std::vector<uint8_t>& bytes,
                                 size_t& doubleRoundedSteps, size_t& shapedGlyphs) {
  using freeink::font::FtFont;
  FtFont reference;
  assert(reference.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 1));
  FtFont::RenderOptions options;
  options.hinting = FtFont::HintingMode::Auto;
  assert(reference.setRenderOptions(options));
  for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
    const auto* data = adapter.atSize(points)->data;
    const auto checkGlyph = [&](uint32_t cp) {
      const auto* glyph = data->dynamicGlyphHandler(data->glyphMissCtx, cp);
      assert(glyph);
      // Ligatures can use a GSUB glyph ID rather than a Unicode cmap entry.
      const uint32_t id = glyph->dataOffset & 0x80000000u ? glyph->dataOffset & 0x7fffffffu : reference.glyphId(cp);
      if (glyph->dataOffset & 0x80000000u) ++shapedGlyphs;
      FtFont::GlyphMetrics metrics;
      assert(reference.metricsGlyph26_6(id, scalableFontPixelSize26_6(points), metrics));
      const auto expected = std::clamp<int32_t>(metrics.advance26_6 >> 2, 0, UINT16_MAX);
      assert(glyph->advanceX == expected);
      // A second lookup exercises the cached metric, including blank spaces.
      assert(data->dynamicGlyphHandler(data->glyphMissCtx, cp)->advanceX == expected);
      if (fp4::toPixel((metrics.advance26_6 + 2) >> 2) != fp4::toPixel(expected)) ++doubleRoundedSteps;
      for (const uint32_t right : {'A', 'V', 'T', 'o'}) {
        const auto rawKern =
            reference.kerningGlyphs26_6(id, reference.glyphId(right), scalableFontPixelSize26_6(points));
        const int64_t wide = rawKern;
        const auto expectedKern = std::clamp<int32_t>(wide >= 0 ? (wide + 2) / 4 : -((-wide + 2) / 4), -128, 127);
        const auto kern = data->kerningHandler(data->glyphMissCtx, cp, right);
        assert(kern == expectedKern);
        assert(data->kerningHandler(data->glyphMissCtx, cp, right) == expectedKern);
        // Compare against a single pixel snap of SDK metrics plus existing fp4 kerning.
        assert(fp4::toPixel(expected + kern) == (metrics.advance26_6 + kern * 4 + 32) >> 6);
      }
    };
    for (uint32_t cp = 32; cp < 127; ++cp) checkGlyph(cp);
    for (uint32_t cp = 0xfb00; cp <= 0xfb04; ++cp) checkGlyph(cp);
  }
}

int main(int argc, char** argv) {
  assert(argc >= 2);
  // Match the reader's eight built-in faces, which stay alive beside SD fonts.
  const char* names[] = {"bitter_regular",     "bitter_bold",     "bitter_italic",     "bitter_bolditalic",
                         "lexenddeca_regular", "lexenddeca_bold", "lexenddeca_italic", "lexenddeca_bolditalic"};
  std::array<std::vector<uint8_t>, 8> bytes;
  std::array<HalScalableFont, 8> builtin;
  for (size_t i = 0; i < 8; ++i) {
    std::ifstream file(std::string(argv[1]) + "/" + names[i] + ".ttf", std::ios::binary);
    bytes[i] = {std::istreambuf_iterator<char>(file), {}};
    assert(builtin[i].openMemory(bytes[i].data(), bytes[i].size()));
    assert(builtin[i].probeGlyph('T', 12));
  }
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  size_t doubleRoundedSteps = 0;
  size_t shapedGlyphs = 0;
  for (size_t i = 0; i < builtin.size(); ++i) {
    checkRuntimeAdvances(builtin[i], bytes[i], doubleRoundedSteps, shapedGlyphs);
  }
  assert(doubleRoundedSteps > 0);
  assert(shapedGlyphs > 0);
  assert(HalScalableFont::renderingRevision() == 10);
  std::printf("ADVANCE regression cases=%zu shaped glyphs=%zu\n", doubleRoundedSteps, shapedGlyphs);
  uint32_t residentIdentity = 0;
  uint64_t residentPixels = 0;
  uint32_t temporaryIdentity = 0;
  for (FileMode mode : {FileMode::Auto, FileMode::Stream, FileMode::Temporary}) {
    std::array<HalScalableFont, 4> family;
    const auto start = std::chrono::steady_clock::now();
    storageReadCalls = storageReadBytes = 0;
    for (int i = 2; i < argc; ++i) {
      assert(i - 2 < 4);
      assert(family[i - 2].openFile(argv[i], HalScalableFont::MaxFamilyBytes, options, mode));
    }
    if (argc > 2) {
      if (mode == FileMode::Auto)
        residentIdentity = family[0].fingerprint();
      else
        assert(residentIdentity != family[0].fingerprint());
      if (mode == FileMode::Temporary) temporaryIdentity = family[0].fingerprint();
      const auto identity = family[0].fingerprint();
      assert(family[0].setRenderOptions(options));
      assert(identity == family[0].fingerprint());
    }
    uint64_t pixelHash = 14695981039346656037ull;
    const auto addHash = [&](uint64_t value) { pixelHash = (pixelHash ^ value) * 1099511628211ull; };
    const auto openedReads = storageReadCalls;
    const auto openedBytes = storageReadBytes;
    if (mode == FileMode::Temporary) {
      size_t totalFileBytes = 0;
      // Opening now includes a bounded static-outline table-directory preflight.
      for (int i = 2; i < argc; ++i) totalFileBytes += family[i - 2].fileBytes() + 12 + 16 * tableCount(argv[i]);
      if (argc > 2) assert(openedBytes <= totalFileBytes);
    }
    const auto renderGlyph = [&](int i, unsigned cp, unsigned size) {
      if (!family[i - 2].hasCodepoint(cp)) return true;
      const auto* data = family[i - 2].atSize(size)->data;
      const auto* glyph = data->dynamicGlyphHandler(data->glyphMissCtx, cp);
      const auto* bitmap =
          glyph && glyph->width && glyph->height ? data->bitmapHandler(data->glyphMissCtx, glyph) : nullptr;
      if (!glyph || (glyph->width && glyph->height && !bitmap)) {
        std::printf("FAIL: %s U+%04X @%u streamed=%d\n", argv[i], cp, size, mode != FileMode::Auto);
        return false;
      }
      addHash(glyph->width);
      addHash(glyph->height);
      addHash(glyph->advanceX);
      addHash(glyph->left);
      addHash(glyph->top);
      for (size_t b = 0; b < (size_t(glyph->width) * glyph->height + 3) / 4; ++b) addHash(bitmap[b]);
      return true;
    };
    // Include first-use auto-hinting and glyph reads, not just face setup, in
    // the dictionary I/O sample. Exercise every available style at one size.
    constexpr char definition[] = "Example (noun): a thing characteristic of its kind, or illustrating a rule.";
    for (int i = 2; i < argc; ++i) {
      for (const char* cp = definition; *cp; ++cp)
        if (!renderGlyph(i, static_cast<unsigned char>(*cp), 12)) return 1;
    }
    // All fixture faces fit in the optional prefix. Streamed glyph faults
    // should reuse the bytes read during the content-hash scan.
    if (mode == FileMode::Stream) assert(storageReadCalls == openedReads);
    std::printf("DEFINITION mode=%d reads=%zu bytes=%zu\n", int(mode), storageReadCalls, storageReadBytes);
    for (int i = 2; i < argc; ++i) {
      for (unsigned size : {12u, 16u, 22u, 32u, 44u, 12u}) {
        for (unsigned cp = 32; cp < 127; ++cp)
          if (!renderGlyph(i, cp, size)) return 1;
      }
    }
    if (mode == FileMode::Auto)
      residentPixels = pixelHash;
    else
      assert(residentPixels == pixelHash);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("BENCH mode=%d open reads=%zu bytes=%zu total reads=%zu bytes=%zu duration=%lldms\n", int(mode),
                openedReads, openedBytes, storageReadCalls, storageReadBytes, (long long)ms);
  }
  assert(openStorageFiles == 0);
  if (argc > 2) {
    HalScalableFont reopened;
    assert(reopened.openFile(argv[2], HalScalableFont::MaxFileBytes, options, FileMode::Temporary));
    assert(reopened.fingerprint() != temporaryIdentity);
    assert(reopened.fingerprint() != residentIdentity);
  }
  const std::string path = std::string(argv[1]) + "/bitter_regular.ttf";
  testHeap = {8 * 1024 * 1024, 1024, 1024};
  assert(!HalScalableFont::prepareFamily(1024, 4));
  testHeap = {8 * 1024 * 1024, 2 * 1024 * 1024, 2 * 1024 * 1024};
  assert(HalScalableFont::prepareFamily(HalScalableFont::MaxFamilyBytes, 4));
  // Per-face residency keeps regular text in RAM when a later face needs to
  // stream. Reserve includes the three styles still waiting to be opened.
  {
    HalScalableFont regular;
    assert(regular.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Auto, 3));
    assert(openStorageFiles == 0);
    testHeap.largest = 1024;
    HalScalableFont other;
    assert(other.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Auto, 2));
    assert(openStorageFiles == 1);
    const auto reads = storageReadCalls;
    assert(regular.probeGlyph('T', 12));
    assert(storageReadCalls == reads);
    assert(other.probeGlyph('T', 12));
  }
  // A fragmented heap streams the face even though total free memory is ample.
  testHeap.largest = 1024;
  {
    HalScalableFont streamed;
    assert(streamed.openFile(path.c_str()));
    assert(openStorageFiles == 1);
    assert(streamed.probeGlyph('T', 12));
  }
  {
    HalScalableFont temporary;
    assert(temporary.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Temporary));
    assert(openStorageFiles == 1);
    assert(temporary.probeGlyph('T', 12));
  }
  assert(openStorageFiles == 0);
  testHeap = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  // Fail the very first file read, then verify cleanup and the next load.
  failStorageRead = storageReadCalls + 1;
  {
    HalScalableFont failed;
    assert(!failed.openFile(path.c_str()));
    assert(!failed.lastFailureLooksLikeFontData());
  }
  assert(openStorageFiles == 0);
  failStorageRead = 0;
  {
    HalScalableFont recovered;
    assert(recovered.openFile(path.c_str()));
    assert(recovered.probeGlyph('T', 12));
  }
  // Fail a read-ahead during face setup: the exact read must recover without
  // latching a font-data failure. Disable the optional prefix for these fault
  // injections so the callback has to visit the SD read windows.
  testHeap.largest = 1024;
  // Full-file hashing precedes the first window.
  size_t fileBytes = 0;
  assert(HalScalableFont::fileSize(path.c_str(), fileBytes));
  const auto failures = injectedReadFailures;
  failStorageRead = storageReadCalls + 1 + tableCount(path.c_str()) + (fileBytes + 4095) / 4096 + 1;
  {
    HalScalableFont streamed;
    assert(streamed.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Stream));
    assert(streamed.probeGlyph('T', 12));
    assert(!streamed.lastFailureLooksLikeFontData());
  }
  failStorageRead = 0;
  assert(injectedReadFailures == failures + 1);
  assert(openStorageFiles == 0);
  {
    HalScalableFont streamed;
    assert(streamed.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Stream));
    failStorageFrom = storageReadCalls + 1;
    assert(!streamed.probeGlyph('T', 12));
    assert(!streamed.lastFailureLooksLikeFontData());
    failStorageFrom = 0;
  }
  assert(openStorageFiles == 0);
  {
    HalScalableFont recovered;
    assert(recovered.openFile(path.c_str(), HalScalableFont::MaxFileBytes, options, FileMode::Stream));
    assert(recovered.probeGlyph('T', 12));
  }
  assert(openStorageFiles == 0);
  testHeap = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  // Large filename faces must stream without lifting the reader's 2 MiB cap.
  const auto largePath = std::filesystem::temp_directory_path() /
                         ("crossink-filename-font-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ttf");
  {
    std::ofstream large(largePath, std::ios::binary);
    large.write(reinterpret_cast<const char*>(bytes[0].data()), bytes[0].size());
    large.seekp(3 * 1024 * 1024 - 1);
    large.put('\0');
    assert(large.good());
  }
  size_t largeBytes = 0;
  assert(!HalScalableFont::fileSize(largePath.c_str(), largeBytes));
  assert(HalScalableFont::fileSize(largePath.c_str(), largeBytes, HalScalableFont::MaxFilenameFileBytes));
  assert(largeBytes == 3 * 1024 * 1024);
  HalScalableFont::Info info;
  assert(!HalScalableFont::inspectFile(largePath.c_str(), info));
  assert(HalScalableFont::inspectFile(largePath.c_str(), info, nullptr, HalScalableFont::MaxFilenameFileBytes));
  {
    HalScalableFont reader;
    assert(!reader.openFile(largePath.c_str()));
    assert(openStorageFiles == 0);
    HalScalableFont filename;
    assert(filename.openFile(largePath.c_str(), HalScalableFont::MaxFilenameFileBytes, options, FileMode::Filename));
    assert(openStorageFiles == 1);
    assert(filename.probeGlyph('T', 8));
    assert(filename.probeGlyph('T', 10));
    assert(filename.probeGlyph('T', 12));
    const auto* streamedData = filename.atSize(12)->data;
    const auto* residentData = builtin[0].atSize(12)->data;
    assert(streamedData->dynamicGlyphHandler(streamedData->glyphMissCtx, 'T')->width ==
           residentData->dynamicGlyphHandler(residentData->glyphMissCtx, 'T')->width);
  }
  assert(openStorageFiles == 0);
  std::filesystem::remove(largePath);
}
