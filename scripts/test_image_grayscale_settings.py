#!/usr/bin/env python3
"""Exercise the production per-book settings serializer and legacy migrations."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
HEADER = (ROOT / 'src/activities/reader/EpubReaderActivity.h').read_text()


def between(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


FIXTURE = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct CrossPointSettings {
  static constexpr uint8_t INDEXING_FULL_SECTION = 1, INDEXING_METHOD_COUNT = 2;
  static constexpr uint8_t MIN_SCREEN_MARGIN = 5, MAX_WORD_SPACING = 8, MAX_CHARACTER_SPACING = 10;
};
enum class PublisherFonts : uint8_t { Off, Accents, All };
enum class EpubRenderMode : uint8_t { CrossInkDefault };
uint8_t normalizeRenderModeRaw(uint8_t v) { return v < 3 ? v : 0; }
uint16_t clampAutoPageTurnIntervalSeconds(uint16_t v) { return std::clamp<uint16_t>(v, 5, 120); }
struct FsFile {
  std::vector<uint8_t>* bytes = nullptr;
  size_t position = 0;
  int read(void* out, size_t n) {
    if (position + n > bytes->size()) return -1;
    std::memcpy(out, bytes->data() + position, n); position += n; return n;
  }
  size_t write(const void* in, size_t n) {
    auto p = static_cast<const uint8_t*>(in); bytes->insert(bytes->end(), p, p + n); return n;
  }
  void close() {}
};
struct StorageFake {
  std::vector<uint8_t> bytes;
  bool exists(const char*) { return !bytes.empty(); }
  bool openFileForRead(const char*, const std::string&, FsFile& f) {
    if (bytes.empty()) return false;
    f.bytes = &bytes; return true;
  }
  bool openFileForWrite(const char*, const std::string&, FsFile& f) {
    bytes.clear(); f.bytes = &bytes; return true;
  }
} Storage;
'''

CASES = r'''
int main() {
  for (uint8_t global : {0, 1, 2, 3}) {
    globalSnapshot.ruledLines = global;
    for (uint8_t book : {0, 1, 2, 3}) {
      Storage.bytes.clear();
      auto data = loadBookReaderSettingsFile("");
      assert(data.readerSettings.ruledLines == global);
      data.readerSettings.ruledLines = book;
      data.readerSettingsOverrideMask = changedReaderSettingsMask(data.readerSettings, globalSnapshot);
      assert(data.readerSettingsOverrideMask == (book == global ? 0 : RULED_LINES_OVERRIDE));
      assert(!(data.readerSettingsOverrideMask & READER_LAYOUT_SETTING_OVERRIDES));
      assert(saveBookReaderSettingsFile("", data));
      assert(loadBookReaderSettingsFile("").readerSettings.ruledLines == book);
    }
    auto data = loadBookReaderSettingsFile("");
    data.readerSettings.twoToneTextAA = 1;
    data.readerSettingsOverrideMask = TWO_TONE_TEXT_AA_OVERRIDE;
    assert(saveBookReaderSettingsFile("", data));
    Storage.bytes[0] = 15;
    Storage.bytes.pop_back();
    const auto old = loadBookReaderSettingsFile("");
    assert(old.readerSettingsOverrideMask == TWO_TONE_TEXT_AA_OVERRIDE);
    assert(old.readerSettings.ruledLines == global && old.readerSettings.twoToneTextAA == 1);
  }
  for (uint8_t global : {0, 1}) {
    globalSnapshot.twoToneTextAA = global;
    Storage.bytes.clear();
    auto data = loadBookReaderSettingsFile("");
    assert(data.readerSettings.twoToneTextAA == global);
    data.readerSettings.twoToneTextAA = !global;
    data.readerSettingsOverrideMask = changedReaderSettingsMask(data.readerSettings, globalSnapshot);
    assert(data.readerSettingsOverrideMask == TWO_TONE_TEXT_AA_OVERRIDE);
    assert(!(data.readerSettingsOverrideMask & READER_LAYOUT_SETTING_OVERRIDES));
    assert(saveBookReaderSettingsFile("", data));
    assert(Storage.bytes[0] == 16);
    assert(loadBookReaderSettingsFile("").readerSettings.twoToneTextAA == !global);

    // A v14 book with custom darkness inherits the current AA quality.
    data.readerSettingsOverrideMask = TEXT_DARKNESS_OVERRIDE;
    data.readerSettings.textDarkness = 2;
    assert(saveBookReaderSettingsFile("", data));
    Storage.bytes[0] = 14;
    Storage.bytes.resize(Storage.bytes.size() - 2);
    auto old = loadBookReaderSettingsFile("");
    assert(old.readerSettingsOverrideMask == TEXT_DARKNESS_OVERRIDE);
    assert(old.readerSettings.twoToneTextAA == global);
    assert(old.readerSettings.textDarkness == 2);
  }
  for (uint8_t global : {0, 1, 2}) {
    globalSnapshot.textDarkness = global;
    for (uint8_t book : {0, 1, 2}) {
      Storage.bytes.clear();
      auto data = loadBookReaderSettingsFile("");
      assert(data.readerSettings.textDarkness == global);
      data.readerSettings.textDarkness = book;
      data.readerSettingsOverrideMask = TEXT_DARKNESS_OVERRIDE;
      assert(saveBookReaderSettingsFile("", data));
      assert(Storage.bytes[0] == 16);
      auto loaded = loadBookReaderSettingsFile("");
      assert(loaded.readerSettings.textDarkness == book);
      assert(changedReaderSettingsMask(data.readerSettings, globalSnapshot) ==
             (book == global ? 0 : TEXT_DARKNESS_OVERRIDE));
      assert(!(loaded.readerSettingsOverrideMask & READER_LAYOUT_SETTING_OVERRIDES));
      // Version 13 fixture retains publisher-font override and inherits darkness.
      Storage.bytes.resize(Storage.bytes.size() - 3);
      Storage.bytes[0] = 13;
      Storage.bytes[155] = 0x20; // bit 21
      Storage.bytes.back() = 2; // publisherFonts = All
      loaded = loadBookReaderSettingsFile("");
      assert(loaded.readerSettings.publisherFonts == 2);
      assert(loaded.readerSettings.textDarkness == global);
      assert(!(loaded.readerSettingsOverrideMask & TEXT_DARKNESS_OVERRIDE));
    }
  }
  Storage.bytes.clear();
  globalSnapshot.textDarkness = 2;

  for (uint8_t global : {0, 1}) {
    globalSnapshot.imageGrayscale = global;
    Storage.bytes.clear();
    assert(loadBookReaderSettingsFile("").readerSettings.imageGrayscale == global);
    for (uint8_t book : {0, 1}) {
      auto data = loadBookReaderSettingsFile("");
      data.readerSettings.imageGrayscale = book;
      data.readerSettingsOverrideMask = IMAGE_GRAYSCALE_OVERRIDE;
      assert(saveBookReaderSettingsFile("", data));
      auto loaded = loadBookReaderSettingsFile("");
      assert(loaded.readerSettings.imageGrayscale == book);
      assert(loaded.readerSettingsOverrideMask == IMAGE_GRAYSCALE_OVERRIDE);
      assert((loaded.readerSettingsOverrideMask & READER_LAYOUT_SETTING_OVERRIDES) == 0);
      // Removing the override makes the book follow the current global value.
      loaded.readerSettingsOverrideMask = 0;
      assert(saveBookReaderSettingsFile("", loaded));
      assert(loadBookReaderSettingsFile("").readerSettings.imageGrayscale == global);
    }
    // Independent historical fixture: version 9/10 record with a custom font and margins.
    // The byte positions come from the documented file format, not today's writer.
    for (uint8_t version : {9, 10}) {
      Storage.bytes.assign(version == 10 ? 157 : 153, 0);
      Storage.bytes[0] = version;
      Storage.bytes[1] = READER_SETTINGS_FLAG_CUSTOM | READER_SETTINGS_FLAG_AUTO_PAGE_TURN;
      Storage.bytes[2] = 30;
      Storage.bytes[5] = 2; // font family
      Storage.bytes[6] = 18; // point size
      Storage.bytes[7] = 100;
      Storage.bytes[10] = 15; // vertical margin
      Storage.bytes[11] = 20; // horizontal margin
      Storage.bytes[13] = 1; // alignment
      Storage.bytes[23] = 1; // indexing method
      std::memcpy(Storage.bytes.data() + 24, "OldFont", 8);
      if (version == 10) Storage.bytes[155] = 4; // bit 18 = historical SD font override
      auto loaded = loadBookReaderSettingsFile("");
      assert(loaded.hasCustomReaderSettings && loaded.hasAutoPageTurnInterval);
      assert(loaded.autoPageTurnSeconds == 30);
      assert(loaded.readerSettings.imageGrayscale == global);
      assert(!(loaded.readerSettingsOverrideMask & IMAGE_GRAYSCALE_OVERRIDE));
      assert(!(loaded.readerSettingsOverrideMask & TEXT_DARKNESS_OVERRIDE));
      assert(loaded.readerSettings.textDarkness == 2);
      assert(loaded.readerSettings.twoToneTextAA == globalSnapshot.twoToneTextAA);
      assert(!(loaded.readerSettingsOverrideMask & TWO_TONE_TEXT_AA_OVERRIDE));
      assert(loaded.readerSettingsOverrideMask & SD_FONT_FAMILY_OVERRIDE);
      assert(std::strcmp(loaded.readerSettings.sdFontFamilyName, "OldFont") == 0);
      if (version == 9) {
        assert(loaded.readerSettings.readerFontPointSize == 18);
        assert(loaded.readerSettings.screenMarginHorizontal == 20);
      }
      assert(saveBookReaderSettingsFile("", loaded));
      auto migrated = loadBookReaderSettingsFile("");
      assert(migrated.readerSettingsOverrideMask == loaded.readerSettingsOverrideMask);
      assert(migrated.readerSettings.imageGrayscale == global);
      assert(std::strcmp(migrated.readerSettings.sdFontFamilyName, "OldFont") == 0);
    }
  }
  globalSnapshot.imageGrayscale = 1;
  BookReaderSettingsData data;
  data.readerSettings.imageGrayscale = 0;
  data.readerSettingsOverrideMask = IMAGE_GRAYSCALE_OVERRIDE;
  assert(saveBookReaderSettingsFile("", data));
  const auto validBytes = Storage.bytes;
  Storage.bytes.back() = 4; // Reject an out-of-range ruling.
  assert(!loadBookReaderSettingsFile("").hasCustomReaderSettings);
  Storage.bytes = validBytes;
  Storage.bytes[Storage.bytes.size() - 2] = 2; // Reject an out-of-range two-tone setting.
  assert(!loadBookReaderSettingsFile("").hasCustomReaderSettings);
  Storage.bytes = validBytes;
  Storage.bytes[Storage.bytes.size() - 3] = 3; // Reject an out-of-range darkness value.
  assert(!loadBookReaderSettingsFile("").hasCustomReaderSettings);
  Storage.bytes = validBytes;
  Storage.bytes.pop_back(); // Truncated new byte must fall back safely.
  assert(!loadBookReaderSettingsFile("").hasCustomReaderSettings);
  ReaderSettingsSnapshot current = globalSnapshot;
  current.imageGrayscale = 0;
  assert(changedReaderSettingsMask(current, globalSnapshot) == IMAGE_GRAYSCALE_OVERRIDE);
}
'''


def main():
    structs = between(HEADER, '  struct ReaderSettingsSnapshot {', '\n private:')
    constants = between(SOURCE, 'constexpr uint8_t LEGACY_READER_SETTINGS_FILE_VERSION', 'constexpr unsigned long RENDER_MODE_TOAST_MS')
    io = between(SOURCE, 'bool readExact(', 'void captureReaderSettings(')
    production = between(SOURCE, 'using ReaderSettingsSnapshot =', 'bool saveBookRenderModeForCache(')
    setup = '''
EpubReaderActivity::ReaderSettingsSnapshot globalSnapshot;
struct { char dictionarySdFontFamilyName[64] = ""; uint8_t dictionaryFontPointSize = 0; } SETTINGS;
void captureReaderSettings(EpubReaderActivity::ReaderSettingsSnapshot& out) { out = globalSnapshot; }
'''
    program = FIXTURE + 'struct EpubReaderActivity {\n' + structs + '\nenum class BookSettingsReadStatus : uint8_t { Missing, Loaded, Invalid };\n};\n' + constants + io + setup + production + CASES
    with tempfile.TemporaryDirectory(prefix='crossink-image-settings-') as directory:
        source = Path(directory) / 'settings.cpp'
        binary = Path(directory) / 'settings'
        source.write_text(program)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-const-variable', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: ruled lines round trips, version 15 inheritance, two-tone AA round trips, version 14 inheritance, text darkness round trips, version 13 inheritance, grayscale overrides, inheritance, version 9/10 migrations, font preservation, truncated records')


if __name__ == '__main__':
    main()
