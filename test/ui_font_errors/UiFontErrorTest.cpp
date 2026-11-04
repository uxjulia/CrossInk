#include <CrossPointSettings.h>
#include <FilenameFontSystem.h>
#include <GfxRenderer.h>
#include <HalScalableFont.h>
#include <HalStorage.h>
#include <Memory.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Scale policy and settings persistence are outside this font-loader test.
uint8_t CrossPointSettings::defaultUiScale() { return UI_SCALE_SMALL; }

// Registry/cache persistence is outside this test; use its file-list lookup contract.
const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& file : files)
    if (file.pointSize == size && file.style == style) return &file;
  return nullptr;
}

int main(int argc, char** argv) {
  assert(argc >= 2);
  const std::filesystem::path repo = argv[1];
  char temp[] = "/tmp/crossink-font-errors-XXXXXX";
  testRoot = mkdtemp(temp);
  const auto put = [&](const char* family, const char* name, const std::filesystem::path& source) {
    const auto dir = std::filesystem::path(testRoot) / ".crossink/languages/fonts" / family;
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(source, dir / name, std::filesystem::copy_options::overwrite_existing);
  };
  const auto regular = repo / "lib/EpdFont/scalableFonts/bitter_regular.ttf";
  const auto bold = repo / "lib/EpdFont/scalableFonts/bitter_bold.ttf";
  const auto foreign = std::filesystem::path(testRoot) / ".crosspoint/languages/fonts/CrossPoint";
  std::filesystem::create_directories(foreign);
  std::filesystem::copy_file(regular, foreign / "Regular.ttf");
  put("Good", "Regular.ttf", regular);
  put("Bold Only", "Bold.ttf", bold);
  put("Duplicate", "One.ttf", regular);
  put("Duplicate", "Two.ttf", regular);
  put("Sidecars", "._Ghost.ttf", regular);
  put("Good", "._Regular.ttf", regular);
  const auto corruptDir = std::filesystem::path(testRoot) / ".crossink/languages/fonts/Corrupt";
  std::filesystem::create_directories(corruptDir);
  std::ofstream(corruptDir / "Bad.ttf") << "not a font file";
  const auto largeDir = std::filesystem::path(testRoot) / ".crossink/languages/fonts/Large";
  std::filesystem::create_directories(largeDir);
  {
    std::ofstream file(largeDir / "Large.otf", std::ios::binary);
    file.seekp(HalScalableFont::MaxFilenameFileBytes);
    file.put('\0');
  }

  GfxRenderer renderer;
  FilenameFontSystem system;
  std::vector<FilenameFontSystem::Family> families;
  assert(system.discover(families));
  assert(std::none_of(families.begin(), families.end(), [](const auto& entry) { return entry.name == "CrossPoint"; }));
  const auto errorFor = [&](const char* name) {
    const auto found =
        std::find_if(families.begin(), families.end(), [&](const auto& entry) { return entry.name == name; });
    assert(found != families.end());
    return found->error;
  };
  assert(errorFor("Good") == FontLoadError::None);
  assert(errorFor("Bold Only") == FontLoadError::InvalidFamily);
  assert(errorFor("Duplicate") == FontLoadError::InvalidFamily);
  assert(errorFor("Corrupt") == FontLoadError::Unsupported);
  assert(errorFor("Large") == FontLoadError::TooLarge);
  assert(std::none_of(families.begin(), families.end(), [](const auto& entry) { return entry.name == "Sidecars"; }));
  const auto select = [&](const char* name) {
    std::strcpy(SETTINGS.filenameFallbackFont, name);
    system.invalidate();
    return system.ensureLoaded(renderer);
  };
  assert(select("Good"));
  assert(renderer.bindings == 3 && system.lastError() == FontLoadError::None);
  assert(!select("Corrupt") && system.lastError() == FontLoadError::Unsupported);
  assert(renderer.bindings == 0);
  assert(!select("Bold Only") && system.lastError() == FontLoadError::InvalidFamily);
  assert(!select("Duplicate") && system.lastError() == FontLoadError::InvalidFamily);
  assert(!select("Large") && system.lastError() == FontLoadError::TooLarge);
  assert(!select("Missing") && system.lastError() == FontLoadError::Read);

  // Font file is valid but the PSRAM reserve cannot be satisfied. This must not
  // be reported as malformed data, and a later successful load clears it.
  testHeap = {0, 0, 0};
  assert(!select("Good") && system.lastError() == FontLoadError::Memory);
  assert(renderer.bindings == 0);
  testHeap = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
  assert(select("Good") && system.lastError() == FontLoadError::None);
  assert(system.discover(families));  // Inspecting the selected face must not reopen it.
  assert(errorFor("Good") == FontLoadError::None);
  system.release(renderer);

  FontLoadError error;
  bool unavailable = false;
  HalScalableFont::Info info;
  assert(
      !HalScalableFont::inspectFile("/missing.ttf", info, &unavailable, HalScalableFont::MaxFilenameFileBytes, &error));
  assert(error == FontLoadError::Read && unavailable);
  assert(!HalScalableFont::inspectFile("/.crossink/languages/fonts/Corrupt/Bad.ttf", info, &unavailable,
                                       HalScalableFont::MaxFilenameFileBytes, &error));
  assert(error == FontLoadError::Unsupported && !unavailable);

  failOpenWrapperAlloc = true;
  assert(!system.discover(families) && system.lastError() == FontLoadError::Memory);
  assert(!HalScalableFont::inspectFile("/.crossink/languages/fonts/Good/Regular.ttf", info, &unavailable,
                                       HalScalableFont::MaxFilenameFileBytes, &error));
  assert(error == FontLoadError::Memory && unavailable);
  size_t bytes = 0;
  assert(!HalScalableFont::fileSize("/.crossink/languages/fonts/Good/Regular.ttf", bytes,
                                    HalScalableFont::MaxFilenameFileBytes, &error));
  assert(error == FontLoadError::Memory);
  {
    HalScalableFont font;
    assert(!font.openFile("/.crossink/languages/fonts/Good/Regular.ttf") &&
           font.lastLoadError() == FontLoadError::Memory);
  }
  failOpenWrapperAlloc = false;
  failDirectoryWrapperAlloc = true;
  assert(!system.discover(families) && system.lastError() == FontLoadError::Memory);
  assert(!select("Good") && system.lastError() == FontLoadError::Memory);
  failDirectoryWrapperAlloc = false;
  failDirectoryScan = true;
  assert(!system.discover(families) && system.lastError() == FontLoadError::Read);
  assert(!select("Good") && system.lastError() == FontLoadError::Read);
  failDirectoryScan = false;
  assert(select("Good"));
  system.release(renderer);

  failFontRead = true;
  assert(!HalScalableFont::inspectFile("/.crossink/languages/fonts/Good/Regular.ttf", info, &unavailable,
                                       HalScalableFont::MaxFilenameFileBytes, &error));
  assert(error == FontLoadError::Read && unavailable);
  assert(!select("Good") && system.lastError() == FontLoadError::Read);
  failFontRead = false;
  assert(select("Good"));
  system.release(renderer);

  // Optional external regression fixture: the exact full Noto OTF from the
  // user's report, kept outside the repository rather than adding a 16 MB asset.
  if (argc > 2) {
    put("Noto", "Regular.otf", argv[2]);
    assert(system.discover(families));
    assert(errorFor("Noto") == FontLoadError::Memory);
    assert(errorFor("Good") == FontLoadError::None);
    assert(!select("Noto") && system.lastError() == FontLoadError::Memory);
    // An OOM during one inspection must not taint the next format decision.
    assert(!HalScalableFont::inspectFile("/.crossink/languages/fonts/Corrupt/Bad.ttf", info, &unavailable,
                                         HalScalableFont::MaxFilenameFileBytes, &error));
    assert(error == FontLoadError::Unsupported && !unavailable);
    assert(select("Good"));
  }
  system.release(renderer);
  assert(system.lastError() == FontLoadError::None);
  std::filesystem::remove_all(testRoot);
  std::puts("UI font discovery, failure reasons, independent families and recovery passed");
}
