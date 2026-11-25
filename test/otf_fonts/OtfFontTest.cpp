#include <FontCatalogIndex.h>
#include <FontInstaller.h>
#include <HalStorage.h>
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#include <TrueTypeFormat.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using Bytes = std::vector<uint8_t>;
static Bytes read(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
static void put(const char* path, const Bytes& bytes) {
  const auto dest = testRoot + path;
  std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
  std::ofstream f(dest, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  assert(f.good());
}
#if CROSSINK_SCALABLE_FONTS
static void put32(Bytes& b, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) b[at + i] = value >> (24 - 8 * i);
}
static void retag(Bytes& b, const char* oldTag, const char* newTag) {
  const size_t count = (unsigned(b[4]) << 8) | b[5];
  bool found = false;
  std::vector<std::array<uint8_t, 16>> records(count);
  for (size_t i = 0; i < count; ++i) {
    std::memcpy(records[i].data(), b.data() + 12 + i * 16, 16);
    if (!std::memcmp(records[i].data(), oldTag, 4)) {
      std::memcpy(records[i].data(), newTag, 4);
      found = true;
    }
  }
  assert(found);
  std::sort(records.begin(), records.end());
  for (size_t i = 0; i < count; ++i) std::memcpy(b.data() + 12 + i * 16, records[i].data(), 16);
}
static void rejected(const Bytes& bytes) {
  const char* path = "/fonts/Rejected/Bad.otf";
  put(path, bytes);
  HalScalableFont::Info info;
  bool unavailable = true;
  assert(!HalScalableFont::inspectFile(path, info, &unavailable));
  assert(!unavailable);  // Bad input must not poison the entire catalog as an I/O failure.
  for (auto mode : {HalScalableFont::FileMode::Auto, HalScalableFont::FileMode::Stream}) {
    HalScalableFont font;
    assert(!font.openFile(path, HalScalableFont::MaxFileBytes, {}, mode));
  }
  assert(!FontInstaller::finishUpload(path, true));
  assert(!Storage.exists(path));
}
#endif
int main(int argc, char** argv) {
  assert(argc == 3);
  char temp[] = "/tmp/crossink-otf-XXXXXX";
  testRoot = mkdtemp(temp);
  assert(!testRoot.empty());
  const std::string fonts = std::string(argv[1]) + "/lib/EpdFont/scalableFonts/";
  const Bytes valid = read(fonts + "bitter_regular.ttf");
  const Bytes cff = read(argv[2]);
  assert(valid.size() > 100 && cff.size() > 100);
  assert(!std::memcmp(cff.data(), "OTTO", 4));
  const char* names[] = {"Regular.otf", "Bold.OTF", "Italic.TtF", "BoldItalic.oTf"};
  const char* sources[] = {"bitter_regular.ttf", "bitter_bold.ttf", "bitter_italic.ttf", "bitter_bolditalic.ttf"};
  for (int i = 0; i < 4; ++i) {
    assert(FontInstaller::isValidCpfontFilename(names[i]) == bool(CROSSINK_SCALABLE_FONTS));
    put((std::string("/fonts/Bitter/") + names[i]).c_str(), read(fonts + sources[i]));
  }
  for (const char* name : {"../Bad.otf", "A/Bad.otf", "A\\Bad.otf", "Bad.otf.tmp", ".otf", "Bad.ttc"})
    assert(!FontInstaller::isValidCpfontFilename(name));
  assert(FontInstaller::isValidCpfontFilename("Bitmap_14.cpfont"));
  put("/fonts/Bitmap/Bitmap_14.cpfont", Bytes{'C', 'P', 'F', 'O', 'N', 'T', 0, 0});
  assert(FontInstaller::validateCpfontFile("/fonts/Bitmap/Bitmap_14.cpfont"));
  put("/fonts/Cff/Cff.otf", cff);
  put("/fonts/CffNamedTtf/Cff.ttf", cff);
  SdCardFontRegistry registry;
  assert(registry.loadNames());
  assert(registry.findFamily("Bitmap"));
  assert(bool(registry.findFamily("Cff")) == bool(CROSSINK_SCALABLE_FONTS));
#if CROSSINK_SCALABLE_FONTS
  const auto* family = registry.findFamily("Bitter");
  assert(family && family->isScalable() && family->files.size() == 4);
  for (unsigned style = 0; style < 4; ++style) assert(family->findFile(0, style));
  family->releaseDetails();
  assert(family->files.empty());
  assert(family->ensureDetails() && family->files.size() == 4);
  freeink::font::FtFont::FaceInfo face;
  char familyName[64];
  assert(freeink::font::FtFont::inspectMemory(cff.data(), cff.size(), face, familyName, sizeof(familyName)) ==
         freeink::font::FtFont::InspectResult::Ok);
  // Real OpenType CFF, including a CFF file with a .ttf name. Font Awesome maps its icons in the PUA.
  for (const char* path : {"/fonts/Cff/Cff.otf", "/fonts/CffNamedTtf/Cff.ttf"}) {
    HalScalableFont::Info info;
    assert(HalScalableFont::inspectFile(path, info));
    assert(FontInstaller::validateCpfontFile(path));
    for (auto mode : {HalScalableFont::FileMode::Auto, HalScalableFont::FileMode::Stream,
                      HalScalableFont::FileMode::Temporary, HalScalableFont::FileMode::Filename}) {
      HalScalableFont font;
      assert(font.openFile(path, HalScalableFont::MaxFileBytes, {}, mode));
      assert(font.probeGlyph(0xF000, 16));
      const auto* data = font.atSize(16)->data;
      const auto* glyph = data->dynamicGlyphHandler(data->glyphMissCtx, 0xF000);
      assert(glyph && glyph->width && glyph->height);
      assert(data->bitmapHandler(data->glyphMissCtx, glyph));
    }
  }
  for (const auto& file : family->files) {
    assert(FontInstaller::validateCpfontFile(file.path.c_str()));
    for (auto mode : {HalScalableFont::FileMode::Auto, HalScalableFont::FileMode::Stream,
                      HalScalableFont::FileMode::Temporary, HalScalableFont::FileMode::Filename}) {
      HalScalableFont font;
      assert(font.openFile(file.path.c_str(), HalScalableFont::MaxFileBytes, {}, mode));
      for (unsigned size : {8, 12, 22}) {
        assert(font.probeGlyph('A', size));
        const auto* data = font.atSize(size)->data;
        const auto* glyph = data->dynamicGlyphHandler(data->glyphMissCtx, 'A');
        assert(glyph && glyph->width && glyph->height);
        const auto* pixels = data->bitmapHandler(data->glyphMissCtx, glyph);
        assert(pixels);
        bool ink = false;
        for (size_t n = 0; n < (size_t(glyph->width) * glyph->height + 3) / 4; ++n) ink |= pixels[n] != 0;
        assert(ink);
      }
    }
  }
  auto bad = cff;
  std::memcpy(bad.data(), "\0\1\0\0", 4);
  rejected(bad);  // TrueType version without glyf/loca.
  bad = cff;
  retag(bad, "CFF ", "CFF2");
  rejected(bad);
  bad = cff;
  retag(bad, "CFF ", "xxxx");
  rejected(bad);
  bad = valid;
  retag(bad, "glyf", "CFF2");
  rejected(bad);
  bad = valid;
  retag(bad, "GDEF", "fvar");
  rejected(bad);
  bad = valid;
  retag(bad, "GDEF", "gvar");
  rejected(bad);
  bad = valid;
  std::memcpy(bad.data(), "ttcf", 4);
  rejected(bad);
  bad = valid;
  retag(bad, "glyf", "xxxx");
  rejected(bad);
  bad = valid;
  put32(bad, 20, 0xfffffff0);
  rejected(bad);
  bad = valid;
  put32(bad, 24, 0xffffffff);
  rejected(bad);
  bad = valid;
  std::memcpy(bad.data() + 28, bad.data() + 12, 16);
  rejected(bad);
  for (size_t size : {size_t(0), size_t(3), size_t(11), size_t(12), size_t(28), valid.size() - 128}) {
    bad = valid;
    bad.resize(size);
    rejected(bad);
  }
  put("/fonts/Failed/Partial.otf", valid);
  assert(!FontInstaller::finishUpload("/fonts/Failed/Partial.otf", false));
  assert(!Storage.exists("/fonts/Failed/Partial.otf"));
  put("/fonts/Kept/Valid.otf", valid);
  assert(FontInstaller::finishUpload("/fonts/Kept/Valid.otf", true));
  assert(Storage.exists("/fonts/Kept/Valid.otf"));
  std::puts(
      "PASS: real SDK CFF rendering; four styles, four loading modes, three sizes rendered; malformed formats and "
      "cleanup");
#else
  assert(!registry.findFamily("Bitter"));
  assert(!FontInstaller::validateCpfontFile("/fonts/Bitter/Regular.otf"));
  assert(!FontInstaller::validateCpfontFile("/fonts/Bitter/Italic.TtF"));
  std::puts("PASS: C3 gate excludes TTF/OTF, preserves bitmap discovery and validation");
#endif
  std::filesystem::remove_all(testRoot);
}
