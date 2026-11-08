#!/usr/bin/env python3
"""Check that upgrade artwork paths bypass old pixels and agree across callers."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parts = [function("lib/Epub/Epub.cpp", signature) for signature in (
    "void normalizeThumbDimensions(",
    "std::string getThumbBmpPathForDimensions(",
    "std::string getAdaptiveThumbBmpPathForDimensions(",
    "std::string Epub::getCoverBmpPath(",
    "std::string Epub::getThumbBmpPath() const",
    "std::string Epub::getThumbBmpPath(int width, int height) const",
    "std::string Epub::getAdaptiveThumbBmpPath(",
)]
parts += [function("lib/Epub/Epub/blocks/ImageBlock.cpp", "std::string getCachePath(")]
parts += [function("src/RecentBooksStore.cpp", "void refreshEpubCoverPath(")]
parts += [function("src/components/UITheme.cpp", "std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int width")]

with tempfile.TemporaryDirectory(prefix="crossink-image-revision-") as directory:
    directory = Path(directory)
    (directory / "production.inc").write_text("\n".join(parts))
    (directory / "test.cpp").write_text(r'''
#include <cassert>
#include <cstdint>
#include <set>
#include <string>
#include "lib/GfxRenderer/ImageToneProfile.h"
constexpr int kDefaultThumbHeight = 180;
constexpr char kWidthPlaceholder[] = "[WIDTH]";
constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kWidthPlaceholderLength = sizeof(kWidthPlaceholder) - 1;
constexpr size_t kHeightPlaceholderLength = sizeof(kHeightPlaceholder) - 1;
struct Epub {
  std::string cachePath = "/cache";
  Epub(const std::string&, const char*) {}
  std::string getCoverBmpPath(bool, ImageToneProfile) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int, int) const;
  std::string getAdaptiveThumbBmpPath(int, int) const;
};
struct RecentBook {
  std::string path, title, author, coverBmpPath;
};
namespace FsHelpers {
bool hasEpubExtension(const std::string& path) { return path.ends_with(".epub"); }
}
struct {
  std::set<std::string> files;
  bool exists(const char* path) const { return files.contains(path); }
} Storage;
struct UITheme {
  static std::string getCoverThumbPath(const std::string&, int, int, bool);
};
#include "production.inc"
int main() {
  Epub epub("/book.epub", "/.crosspoint");
  // Upgrade with old covers and both historical thumbnail names still on SD.
  Storage.files = {"/cache/cover.bmp", "/cache/cover_crop_picco_image.bmp",
                   "/cache/thumb_120x180.bmp", "/cache/thumb_180.bmp",
                   "/cache/thumb_120x180_fit.bmp", "/cache/img_0_1.pxc"};
  for (bool crop : {false, true}) {
    for (auto tone : {ImageToneProfile::Legacy, ImageToneProfile::Even, ImageToneProfile::PiccoCover}) {
      assert(!Storage.exists(epub.getCoverBmpPath(crop, tone).c_str()));
    }
  }
  const auto thumbnail = epub.getThumbBmpPath(120, 180);
  assert(!Storage.exists(thumbnail.c_str()));
  assert(UITheme::getCoverThumbPath(epub.getThumbBmpPath(), 120, 180, true) == thumbnail);
  assert(!Storage.exists(epub.getAdaptiveThumbBmpPath(120, 180).c_str()));
  assert(!Storage.exists(getCachePath("/cache/img_0_1.jpg").c_str()));
  RecentBook book{"/book.epub", "Saved title", "Saved author", "/cache/thumb_[WIDTH]x[HEIGHT].bmp"};
  refreshEpubCoverPath(book);
  assert(book.path == "/book.epub" && book.title == "Saved title" && book.author == "Saved author");
  assert(book.coverBmpPath == epub.getThumbBmpPath());
  // A concrete old saved path is refreshed too; other formats keep their path.
  book.coverBmpPath = "/cache/thumb_120x180.bmp";
  refreshEpubCoverPath(book);
  assert(book.coverBmpPath == epub.getThumbBmpPath());
  const auto current = book.coverBmpPath;
  refreshEpubCoverPath(book);
  assert(book.coverBmpPath == current);
  book.path = "/book.xtc";
  book.coverBmpPath = "/cache/xtc.bmp";
  refreshEpubCoverPath(book);
  assert(book.coverBmpPath == "/cache/xtc.bmp");
  // Once regenerated, all consumers resolve the same new file.
  Storage.files.insert(thumbnail);
  book.coverBmpPath = current;
  assert(Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, 120, 180, true).c_str()));
}
''')
    executable = directory / "test"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(ROOT),
                    str(directory / "test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: stale artwork bypass, thumbnail producer/consumer agreement, recent-book upgrade")
