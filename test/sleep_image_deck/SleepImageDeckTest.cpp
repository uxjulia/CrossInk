#include <Arduino.h>
#include <HalStorage.h>
#include <ImageFolderIndex.h>
#include <SleepImageDeck.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

void put(const std::string& path, const std::string& contents = "Valid image") {
  fs::create_directories(fs::path(test_storage::root + path).parent_path());
  std::ofstream(test_storage::root + path, std::ios::binary) << contents;
}
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
std::string draw(const std::string& folder, bool png = false, bool validate = false) {
  ImageFolderIndex::Selection selection;
  assert(ImageFolderIndex::selectShuffled(folder, png, validate, selection));
  assert(Storage.exists(selection.path.c_str()));
  assert(ImageFolderIndex::recordSleepAttempt(selection, true));
  assert(test_storage::openReaders.empty());
  return selection.path;
}
fs::path findDeck() {
  for (const auto& entry : fs::directory_iterator(test_storage::root + "/.crosspoint/sleep-image-index"))
    if (entry.path().extension() == ".deck") return entry.path();
  assert(false);
  return {};
}
void reset() {
  assert(test_storage::openReaders.empty());
  fs::remove_all(test_storage::root);
  fs::create_directories(test_storage::root);
  test_storage::reverseDirectory = false;
  test_storage::failIterationAfter = -1;
  test_storage::failWritePath.clear();
  test_storage::failRenameFrom.clear();
  test_storage::failSync = test_storage::failClose = test_storage::failAllocation = false;
  forcedSleepRank = -1;
}
void populate(const std::string& folder, int count) {
  for (int i = 0; i < count; ++i) put(folder + "/" + std::to_string(i) + ".bmp");
}

int main() {
  char temporary[] = "/tmp/crossink-sleep-deck-XXXXXX";
  assert(mkdtemp(temporary));
  test_storage::root = temporary;

  // A PNG failure must retry within the active overlay deck, without showing
  // an already displayed BMP or starting an independent BMP-only rotation.
  reset();
  put("/sleep/A.bmp");
  put("/sleep/B.bmp");
  put("/sleep/C.png");
  ImageFolderIndex::Selection retry;
  forcedSleepRank = 0;
  assert(draw("/sleep", true) == "/sleep/A.bmp");
  forcedSleepRank = 1;
  assert(ImageFolderIndex::selectShuffled("/sleep", true, false, retry));
  assert(retry.path == "/sleep/C.png");
  assert(ImageFolderIndex::recordSleepAttempt(retry, false));
  forcedSleepRank = 0;
  assert(ImageFolderIndex::selectShuffled("/sleep", true, false, retry, true));
  assert(retry.path == "/sleep/B.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, true));
  // The first draw in the next round fails: the retry still must avoid B,
  // which was the previous round's last successfully displayed image.
  forcedSleepRank = 1;
  assert(ImageFolderIndex::selectShuffled("/sleep", true, false, retry));
  assert(retry.path == "/sleep/C.png");
  assert(ImageFolderIndex::recordSleepAttempt(retry, false));
  forcedSleepRank = 0;
  assert(ImageFolderIndex::selectShuffled("/sleep", true, false, retry, true));
  assert(retry.path == "/sleep/A.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, true));
  assert(ImageFolderIndex::selectShuffled("/sleep", true, false, retry, true));
  assert(retry.path == "/sleep/B.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, true));
  // A retry subset can be exhausted while untried PNGs remain. Do not restart
  // a subset deck or silently fall back to an unrestricted scan.
  put("/sleep/D.png");
  forcedSleepRank = 0;
  assert(draw("/sleep", true) == "/sleep/A.bmp");
  assert(draw("/sleep", true) == "/sleep/B.bmp");
  assert(!ImageFolderIndex::selectShuffled("/sleep", true, false, retry, true));
  assert(retry.noEligibleRemaining);
  // Header validation filters candidates without discarding successful draws.
  reset();
  put("/sleep/A.bmp");
  put("/sleep/B.bmp");
  put("/sleep/C.bmp", "Bad BMP");
  forcedSleepRank = 0;
  assert(draw("/sleep") == "/sleep/A.bmp");
  forcedSleepRank = 1;
  assert(ImageFolderIndex::selectShuffled("/sleep", false, false, retry));
  assert(retry.path == "/sleep/C.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, false));
  forcedSleepRank = 0;
  assert(ImageFolderIndex::selectShuffled("/sleep", false, true, retry));
  assert(retry.path == "/sleep/B.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, true));
  std::cout
      << "PNG/BMP fallback, failed first draw, exhausted retry subset and BMP validation retain the active deck\n";

  // Validation discovers all malformed cards, so unseen bad entries must not
  // block a usable wallpaper and cause a default dark screen.
  reset();
  put("/sleep/A.bmp");
  put("/sleep/B.bmp", "Bad BMP");
  put("/sleep/C.bmp", "Bad BMP");
  forcedSleepRank = 0;
  assert(draw("/sleep") == "/sleep/A.bmp");
  assert(ImageFolderIndex::selectShuffled("/sleep", false, false, retry));
  assert(retry.path == "/sleep/B.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, false));
  for (int i = 0; i < 3; ++i) assert(draw("/sleep", false, true) == "/sleep/A.bmp");
  // Learning about several bad cards must retain the next unseen valid card
  // and still avoid a repeated display at the next round's boundary.
  reset();
  put("/sleep/A.bmp");
  put("/sleep/B.bmp");
  put("/sleep/C.bmp", "Bad BMP");
  put("/sleep/D.bmp", "Bad BMP");
  forcedSleepRank = 0;
  assert(draw("/sleep") == "/sleep/A.bmp");
  forcedSleepRank = 1;
  assert(ImageFolderIndex::selectShuffled("/sleep", false, false, retry));
  assert(retry.path == "/sleep/C.bmp");
  assert(ImageFolderIndex::recordSleepAttempt(retry, false));
  forcedSleepRank = 0;
  assert(draw("/sleep", false, true) == "/sleep/B.bmp");
  assert(draw("/sleep", false, true) == "/sleep/A.bmp");
  std::cout << "Multiple malformed BMPs are skipped together without losing valid wallpapers\n";

  for (const int count : {1, 2, 3, 16, 17, 18, 255, 1000}) {
    reset();
    populate("/.sleep", count);
    std::string previous;
    std::vector<std::string> firstCycle, secondCycle;
    for (int cycle = 0; cycle < 3; ++cycle) {
      std::set<std::string> seen;
      for (int i = 0; i < count; ++i) {
        const auto path = draw("/.sleep");
        assert(seen.insert(path).second);
        if (count > 1) assert(path != previous);
        previous = path;
        if (cycle == 0) firstCycle.push_back(path);
        if (cycle == 1) secondCycle.push_back(path);
      }
      assert(seen.size() == static_cast<size_t>(count));
    }
    if (count >= 16) assert(firstCycle != secondCycle);
  }
  std::cout << "Full decks, persistence between calls and no boundary repeats: 1/2/3/16/17/18/255/1000 images\n";

  // No upload notification: additions must be found on the next pick. A changed
  // collection starts a new deck containing all current images.
  reset();
  populate("/sleep", 3);
  draw("/sleep");
  put("/sleep/new.BMP");
  std::set<std::string> afterAdd;
  for (int i = 0; i < 4; ++i) assert(afterAdd.insert(draw("/sleep")).second);
  assert(afterAdd.contains("/sleep/new.BMP"));
  fs::rename(test_storage::root + "/sleep/new.BMP", test_storage::root + "/sleep/renamed.bmp");
  fs::remove(test_storage::root + "/sleep/0.bmp");
  std::set<std::string> afterRename;
  for (int i = 0; i < 3; ++i) assert(afterRename.insert(draw("/sleep")).second);
  assert(afterRename.contains("/sleep/renamed.bmp"));
  assert(!afterRename.contains("/sleep/new.BMP") && !afterRename.contains("/sleep/0.bmp"));
  test_storage::reverseDirectory = true;
  std::set<std::string> reordered;
  for (int i = 0; i < 3; ++i) assert(reordered.insert(draw("/sleep")).second);
  std::cout << "Direct additions, deletions, rename and directory reorder refresh the deck\n";

  // Invalidating after a transfer must not discard progress for an unchanged
  // collection. Each selection reopens its SD checkpoint just as a reboot does.
  reset();
  populate("/.sleep", 17);
  std::set<std::string> acrossTransfer;
  for (int i = 0; i < 17; ++i) {
    assert(acrossTransfer.insert(draw("/.sleep")).second);
    ImageFolderIndex::invalidate();
    assert(fs::exists(findDeck()));
  }
  put("/.sleep/transfer.bmp");
  ImageFolderIndex::invalidateForPath("/.sleep/transfer.bmp");
  std::set<std::string> transferred;
  for (int i = 0; i < 18; ++i) assert(transferred.insert(draw("/.sleep")).second);
  assert(transferred.contains("/.sleep/transfer.bmp"));
  const auto checkpoint = read(findDeck());
  ImageFolderIndex::invalidateForPath("/books/unrelated.epub");
  assert(read(findDeck()) == checkpoint);
  std::cout << "USB/upload invalidation preserves unchanged rotation and discovers changed files\n";

  reset();
  put("/sleep/good.bmp");
  put("/sleep/bad.bmp", "Bad BMP");
  put("/sleep/overlay.PNG");
  put("/sleep/.hidden.bmp");
  put("/sleep/._apple.bmp");
  put("/sleep/notes.txt");
  put("/sleep/nested/inside.bmp");
  ImageFolderIndex::Selection invalid;
  for (int i = 0; i < 2; ++i) {
    assert(ImageFolderIndex::selectShuffled("/sleep", false, false, invalid));
    const bool displayed = invalid.path != "/sleep/bad.bmp";
    assert(ImageFolderIndex::recordSleepAttempt(invalid, displayed));
    if (!displayed) break;
  }
  assert(invalid.path == "/sleep/bad.bmp");
  assert(draw("/sleep", false, true) == "/sleep/good.bmp");
  put("/sleep/bad.bmp", "Valid repaired BMP, larger");
  std::set<std::string> repaired;
  for (int i = 0; i < 2; ++i) assert(repaired.insert(draw("/sleep", false, true)).second);
  assert(repaired.contains("/sleep/bad.bmp"));
  std::set<std::string> overlay;
  for (int i = 0; i < 3; ++i) assert(overlay.insert(draw("/sleep", true, true)).second);
  assert(overlay.contains("/sleep/overlay.PNG"));
  populate("/separate", 3);
  std::set<std::string> independent;
  for (int i = 0; i < 3; ++i) assert(independent.insert(draw("/separate")).second);
  std::cout << "BMP validation/repair, PNG mode, hidden files, subfolders and independent folders\n";

  reset();
  fs::create_directories(test_storage::root + "/sleep");
  ImageFolderIndex::Selection selection;
  assert(!ImageFolderIndex::selectShuffled("/sleep", false, false, selection));
  put("/sleep/first.bmp");
  assert(draw("/sleep") == "/sleep/first.bmp");
  put("/sleep/second.bmp");
  draw("/sleep");
  test_storage::failIterationAfter = 0;
  assert(!ImageFolderIndex::selectShuffled("/sleep", false, false, selection));
  test_storage::failIterationAfter = -1;
  const auto beforeFailure = read(findDeck());
  test_storage::failAllocation = true;
  assert(!ImageFolderIndex::selectShuffled("/sleep", false, false, selection));
  test_storage::failAllocation = false;
  assert(read(findDeck()) == beforeFailure);
  const auto failedCheckpoint = [&] {
    assert(ImageFolderIndex::selectShuffled("/sleep", false, false, selection));
    assert(!ImageFolderIndex::recordSleepAttempt(selection, true));
    assert(read(findDeck()) == beforeFailure);
  };
  test_storage::failWritePath = ".deck.tmp";
  failedCheckpoint();
  test_storage::failWritePath.clear();
  assert(read(findDeck()) == beforeFailure);
  test_storage::failSync = true;
  failedCheckpoint();
  test_storage::failSync = false;
  assert(read(findDeck()) == beforeFailure);
  test_storage::failClose = true;
  failedCheckpoint();
  test_storage::failClose = false;
  assert(read(findDeck()) == beforeFailure);
  const auto deck = findDeck();
  test_storage::failRenameFrom = deck.string().substr(test_storage::root.size()) + ".tmp";
  failedCheckpoint();
  assert(read(deck) == beforeFailure);
  // Model power loss after renaming the old checkpoint to its backup.
  fs::rename(deck, deck.string() + ".bak");
  draw("/sleep");
  assert(!fs::exists(deck.string() + ".bak"));
  put(deck.string().substr(test_storage::root.size()), "truncated checkpoint");
  draw("/sleep");
  auto corrupted = read(deck);
  corrupted.back() ^= 1;
  put(deck.string().substr(test_storage::root.size()), corrupted);
  draw("/sleep");
  assert(test_storage::openReaders.empty());
  std::cout << "Empty folders, failed scan/allocation/write/sync/close/rename and interrupted/corrupt checkpoints\n";

  reset();
  Storage.ensureDirectoryExists("/.crosspoint/sleep-image-index");
  test_storage::maxAllocation = 0;
  uint16_t index = 0;
  std::set<uint16_t> large;
  for (int i = 0; i < 50; ++i) {
    assert(SleepImageDeck::select("/.crosspoint/sleep-image-index/large.idx", 42, UINT16_MAX, index) ==
           SleepImageDeck::Result::Selected);
    assert(SleepImageDeck::recordAttempt("/.crosspoint/sleep-image-index/large.idx", 42, UINT16_MAX, index, true));
    assert(large.insert(index).second);
  }
  assert(test_storage::maxAllocation == 8192);
  std::cout << "65535-image deck uses at most 8192 bytes of scratch RAM\n";

  reset();
  populate("/bootscreen", 3);
  uint16_t recent[2] = {0, 1};
  assert(ImageFolderIndex::select("/bootscreen", false, false, recent, 2, 0, 2, 2, selection));
  assert(selection.index == 2);  // Existing boot-screen recent-history policy.
  assert(test_storage::openReaders.empty());
  // Upgrade/corrupt index headers are disposable and must rebuild automatically.
  for (const auto& entry : fs::directory_iterator(test_storage::root + "/.crosspoint/sleep-image-index")) {
    if (entry.path().extension() == ".idx") {
      auto old = read(entry.path());
      old[4] = 1;
      put(entry.path().string().substr(test_storage::root.size()), old);
    }
  }
  assert(ImageFolderIndex::select("/bootscreen", false, false, recent, 2, 0, 2, 2, selection));
  assert(selection.index == 2);
  std::cout << "Index format upgrade preserves the existing boot-screen selection policy\n";
  reset();
  fs::remove_all(test_storage::root);
}
