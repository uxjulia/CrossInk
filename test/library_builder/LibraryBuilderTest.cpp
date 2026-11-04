#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryFileTypes.h"
#include "LibraryIndexFile.h"
#include "LibraryMetadataCache.h"
#include "LibrarySort.h"
#include "LibraryText.h"
#include "Memory.h"

using namespace library;

std::vector<std::string> preservedCacheClears;
bool preserveCacheState = true;

bool clearBookCachePreservingUserState(const std::string& path, const bool sourceChanged) {
  EXPECT_TRUE(sourceChanged);
  preservedCacheClears.push_back(path);
  return preserveCacheState;
}

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

bool recordAtPath(LibraryIndexFile& index, const std::string& path, ClixRecord& out) {
  for (uint16_t ordinal = 0; ordinal < index.bookCount(); ordinal++) {
    ClixRecord record{};
    std::string storedPath;
    if (!index.readRecord(ordinal, record) || !index.readPath(record, storedPath)) return false;
    if (storedPath == path) {
      out = record;
      return true;
    }
  }
  return false;
}

// Firmware that wrote an older index format never wrote the metadata cache,
// so upgrade tests start without one.
void dropMetadataCache() {
  Storage.remove(LibraryMetadataCache::slotPath());
  Storage.remove(LibraryMetadataCache::payloadPath());
}

// Build a genuine V5 name section by removing each V6 position and updating
// record offsets. Changing only the version byte would leave V6 blobs behind.
bool downgradeIndexToVersionFive() {
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  std::vector<uint8_t> old(bytes.begin(), bytes.begin() + header.nameStart);
  for (uint16_t i = 0; i < header.bookCount; i++) {
    ClixRecord record{};
    std::memcpy(&record, bytes.data() + recordOffset(header, i), sizeof(record));
    uint32_t next = header.nameLen;
    if (i + 1 < header.bookCount) {
      ClixRecord following{};
      std::memcpy(&following, bytes.data() + recordOffset(header, i + 1), sizeof(following));
      next = following.nameOff;
    }
    if (next < record.nameOff + sizeof(uint32_t) || next > header.nameLen) return false;
    const uint32_t newOffset = static_cast<uint32_t>(old.size() - header.nameStart);
    old.insert(old.end(), bytes.begin() + header.nameStart + record.nameOff,
               bytes.begin() + header.nameStart + next - sizeof(uint32_t));
    record.nameOff = newOffset;
    std::memcpy(old.data() + recordOffset(header, i), &record, sizeof(record));
  }
  header.formatVersion = 5;
  header.nameLen = static_cast<uint32_t>(old.size() - header.nameStart);
  header.selfSize = static_cast<uint32_t>(old.size());
  std::memcpy(old.data(), &header, sizeof(header));
  bytes = std::move(old);
  return true;
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    fake::psram = false;
    fake::psramAllocations = 0;
    setSortRunCapacityForTesting(0);
    invalidateLibraryIndex();
    bookMetadata.clear();
    cachedBookMetadata.clear();
    metadataCacheUse.clear();
    preservedCacheClears.clear();
    preserveCacheState = true;
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST_F(LibraryBuilderTest, SkipsOnlyExactSystemDirectoriesInAnyFatCasing) {
  for (const char* path : {"/XTCache/fontPos.txt", "/Backup/xtcache/epub/language.txt",
                           "/SYSTEM VOLUME INFORMATION/notes.md", "/Books/System Volume Information/book.epub"})
    fake::add(path);
  for (const char* path : {"/fontPos.txt", "/language.txt", "/XTCache.txt", "/System Volume Information.md",
                           "/XTCacheArchive/book.txt", "/System Volume Information Archive/book.md"})
    fake::add(path);
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 8u);
  for (const char* path : {"/XTCache/fontPos.txt", "/Backup/xtcache/epub", "/SYSTEM VOLUME INFORMATION/notes.md",
                           "/Books/System Volume Information/book.epub"})
    EXPECT_EQ(fake::directoryEntriesByPath[path], 0u) << path;
  ClixRecord record{};
  EXPECT_TRUE(recordAtPath(index, "/XTCacheArchive/book.txt", record));
  EXPECT_TRUE(recordAtPath(index, "/System Volume Information.md", record));
}

TEST_F(LibraryBuilderTest, PreviousScanPolicyRebuildPreservesArrivalAndSourceFiles) {
  // Build the old index with an included folder, then give its on-disk folder
  // entry the system name. V6 and V7 share the same layout.
  fake::add("/ZTCache/fontPos.txt");
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord before{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", before));
  index.close();
  auto& bytes = fake::files[INDEX]->bytes;
  const std::string oldFolder = "/ZTCache";
  auto folder = std::search(bytes.begin(), bytes.end(), oldFolder.begin(), oldFolder.end());
  ASSERT_NE(folder, bytes.end());
  *(folder + 1) = 'X';
  bytes[offsetof(ClixHeader, formatVersion)] = 6;
  for (const char* suffix : {"", "/fontPos.txt"}) {
    const std::string oldPath = oldFolder + suffix;
    fake::files[std::string("/XTCache") + suffix] = fake::files[oldPath];
    fake::files.erase(oldPath);
  }
  ASSERT_FALSE(index.open(INDEX));
  ASSERT_TRUE(index.openForReconciliation(INDEX));
  index.close();
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(stats.indexReplaced);
  // Prior release metadata must be refreshed as part of this rebuild.
  EXPECT_EQ(stats.metadataReused, 0u);
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord after{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", after));
  EXPECT_EQ(before.firstSeen, after.firstSeen);
  EXPECT_EQ(index.bookCount(), 2u);
  EXPECT_EQ(stats.removed, 1u);
  EXPECT_TRUE(fake::files.count("/XTCache/fontPos.txt"));
  EXPECT_TRUE(preservedCacheClears.empty());
}

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, MissingModificationDateClearsDerivedCacheThroughStatePreservingPath) {
  fake::files["/a.epub"]->time = 0;
  initial();
  preservedCacheClears.clear();
  metadataCacheUse.clear();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(preservedCacheClears, std::vector<std::string>{"/a.epub"});
  ASSERT_EQ(metadataCacheUse.size(), 1u);
  EXPECT_FALSE(metadataCacheUse.front());
}

TEST_F(LibraryBuilderTest, ChangedEpubAbortsIndexRefreshIfReadingStateCannotBePreserved) {
  initial();
  fake::files["/a.epub"]->time++;
  preserveCacheState = false;
  fake::parses = 0;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(preservedCacheClears, std::vector<std::string>{"/a.epub"});
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_TRUE(Storage.exists(INDEX));
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, FileTypesKeepMarkdownSeparateAndIncludeXtch) {
  EXPECT_EQ(fileTypeFor("book.epub"), FileEpub);
  EXPECT_EQ(fileTypeFor("book.xtc"), FileXtc);
  EXPECT_EQ(fileTypeFor("book.xtch"), FileXtc);
  EXPECT_EQ(fileTypeFor("book.txt"), FileTxt);
  EXPECT_EQ(fileTypeFor("book.md"), FileMarkdown);
  fake::add("/graphic.xtch");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  EXPECT_TRUE(recordAtPath(index, "/graphic.xtch", record));
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, DirectoryIterationFailureRetainsPreviousIndex) {
  // openNextFile() returning falsy is ambiguous between "reached the end of
  // the directory" and an SdFat allocation/iteration error partway through.
  // A card glitch mid-listing must abort the build, not silently look like a
  // fully (if short) scanned folder.
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::failDirectoryIterationPath = "/";

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, DirectoryOpenFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::failOpenPath = "/";

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, MetadataCacheIsBypassedOnlyForChangedSources) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  ASSERT_EQ(metadataCacheUse.size(), 2u);
  EXPECT_TRUE(metadataCacheUse[0]);
  EXPECT_TRUE(metadataCacheUse[1]);

  metadataCacheUse.clear();
  fake::files["/a.epub"]->time++;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  ASSERT_EQ(metadataCacheUse.size(), 1u);
  EXPECT_FALSE(metadataCacheUse[0]);

  metadataCacheUse.clear();
  fake::files["/b.epub"]->bytes.push_back('x');
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  ASSERT_EQ(metadataCacheUse.size(), 1u);
  EXPECT_FALSE(metadataCacheUse[0]);

  metadataCacheUse.clear();
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(metadataCacheUse.empty());
}

TEST_F(LibraryBuilderTest, MetadataStagingTruncatesAtUtf8Boundaries) {
  const std::string title(254, 'T');
  const std::string author(127, 'A');
  const std::string emojiTitle(252, 'E');
  const std::string emojiAuthor(125, 'B');
  bookMetadata["/a.epub"].title = title + "\xC3\xA9";
  bookMetadata["/a.epub"].author = author + "\xC3\xA9";
  bookMetadata["/b.epub"].title = emojiTitle + "\xF0\x9F\x98\x80";
  bookMetadata["/b.epub"].author = emojiAuthor + "\xF0\x9F\x98\x80";

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord aRecord{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", aRecord));
  std::string storedTitle;
  std::string storedAuthor;
  ASSERT_TRUE(index.readTitle(aRecord, storedTitle));
  ASSERT_TRUE(index.readSourceAuthor(aRecord, storedAuthor));
  EXPECT_EQ(storedTitle, title);
  EXPECT_EQ(storedAuthor, author);

  ClixRecord bRecord{};
  ASSERT_TRUE(recordAtPath(index, "/b.epub", bRecord));
  ASSERT_TRUE(index.readTitle(bRecord, storedTitle));
  ASSERT_TRUE(index.readSourceAuthor(bRecord, storedAuthor));
  EXPECT_EQ(storedTitle, emojiTitle);
  EXPECT_EQ(storedAuthor, emojiAuthor);
}

TEST_F(LibraryBuilderTest, SeriesAndGenreSurviveRebuildWithoutReparsing) {
  bookMetadata["/a.epub"].series = "Earthsea";
  bookMetadata["/a.epub"].seriesIndex = "2.5";
  bookMetadata["/a.epub"].genre = "Fantasy";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", record));
  std::string series;
  std::string genre;
  ASSERT_TRUE(index.readSeries(record, series));
  ASSERT_TRUE(index.readGenre(record, genre));
  EXPECT_EQ(series, "Earthsea");
  EXPECT_EQ(genre, "Fantasy");
  uint32_t position = 0;
  ASSERT_TRUE(index.readSeriesPosition(record, position));
  EXPECT_NE(position, CLIX_UNKNOWN_SERIES_POSITION);
}

TEST_F(LibraryBuilderTest, SeriesSortUsesNumericOrderThenTitleAndPutsMissingNumbersLast) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  fake::add("/e.epub");
  fake::add("/f.epub");
  fake::add("/g.epub");
  for (const char* path : {"/a.epub", "/b.epub", "/c.epub", "/d.epub", "/e.epub", "/f.epub", "/g.epub"})
    bookMetadata[path].series = "Earthsea";
  bookMetadata["/a.epub"].seriesIndex = "10";
  bookMetadata["/b.epub"].seriesIndex = "2.5";
  bookMetadata["/c.epub"].seriesIndex = "2";
  bookMetadata["/d.epub"].seriesIndex = "invalid";
  bookMetadata["/e.epub"].seriesIndex = "2";
  bookMetadata["/f.epub"].seriesIndex = "-1";
  bookMetadata["/g.epub"].seriesIndex = "0";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/f.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 1), "/g.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 2), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 3), "/e.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 4), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 5), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 6), "/d.epub");
}

TEST_F(LibraryBuilderTest, SeriesAndGenreSortByFullFoldedNameWithMissingValuesLast) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  bookMetadata["/a.epub"].series = "Alpha";
  bookMetadata["/a.epub"].genre = "Zeta";
  bookMetadata["/b.epub"].series = "beta";
  bookMetadata["/b.epub"].genre = "Fantasy";
  bookMetadata["/c.epub"].series = "ALPHA";
  bookMetadata["/c.epub"].genre = "fantasy";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 1), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesDesc, 0), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::GenreAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::GenreAsc, 1), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::GenreAsc, 2), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::GenreAsc, 3), "/d.epub");
}

TEST_F(LibraryBuilderTest, SeriesSortRefinesLongSharedPrefixes) {
  const std::string prefix(24, 'a');
  bookMetadata["/a.epub"].series = prefix + "z";
  bookMetadata["/b.epub"].series = prefix + "b";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 1), "/a.epub");
}

TEST_F(LibraryBuilderTest, SeriesPositionSortsAfterANameLongerThanTheKeySegment) {
  const std::string series(48, 'a');
  bookMetadata["/a.epub"].series = series;
  bookMetadata["/b.epub"].series = series;
  bookMetadata["/a.epub"].seriesIndex = "10";
  bookMetadata["/b.epub"].seriesIndex = "2";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 1), "/a.epub");
}

TEST_F(LibraryBuilderTest, VersionThreeIndexReparsesSeriesOrderDuringUpgrade) {
  bookMetadata["/a.epub"].series = "Earthsea";
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();

  // Two-book v3 and v6 indexes have the same aligned nameStart. The v3
  // permutation section ends early, leaving padding before the name blob.
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = 3;
  dropMetadataCache();
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(after.header().formatVersion, CLIX_FORMAT_VERSION);
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
  std::string series;
  ASSERT_TRUE(after.readSeries(rebuilt, series));
  EXPECT_EQ(series, "Earthsea");
}

TEST_F(LibraryBuilderTest, VersionFiveIndexReparsesSeriesPositionsAndKeepsArrivalHistory) {
  bookMetadata["/a.epub"].series = "Earthsea";
  bookMetadata["/b.epub"].series = "Earthsea";
  bookMetadata["/a.epub"].seriesIndex = "2";
  bookMetadata["/b.epub"].seriesIndex = "1";
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();

  ASSERT_TRUE(downgradeIndexToVersionFive());
  dropMetadataCache();
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(pathAt(after, SortOrder::SeriesAsc, 0), "/b.epub");
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
}

TEST_F(LibraryBuilderTest, InterruptedUpgradeKeepsValidOldLiveIndexOverStaleBackup) {
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();
  ASSERT_TRUE(downgradeIndexToVersionFive());

  constexpr char BACKUP[] = "/.crosspoint/library.bak";
  fake::files[BACKUP] = std::make_shared<fake::Node>(*fake::files[INDEX]);
  ClixHeader backupHeader{};
  std::memcpy(&backupHeader, fake::files[BACKUP]->bytes.data(), sizeof(backupHeader));
  for (uint16_t i = 0; i < backupHeader.bookCount; i++) {
    ClixRecord backupRecord{};
    std::memcpy(&backupRecord, fake::files[BACKUP]->bytes.data() + recordOffset(backupHeader, i), sizeof(backupRecord));
    backupRecord.firstSeen = 100 + i;
    std::memcpy(fake::files[BACKUP]->bytes.data() + recordOffset(backupHeader, i), &backupRecord, sizeof(backupRecord));
  }

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(Storage.exists(BACKUP));
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
}

TEST_F(LibraryBuilderTest, OldFoldVersionRebuildsTitleKeysWithoutReparsingBooks) {
  bookMetadata["/a.epub"].title = "The Iliad";
  bookMetadata["/b.epub"].title = "Rendezvous";
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ASSERT_EQ(pathAt(before, SortOrder::TitleAsc, 1), "/a.epub");
  ClixRecord original{};
  ClixRecord other{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  ASSERT_TRUE(recordAtPath(before, "/b.epub", other));
  before.close();

  // Simulate the old index's "the " article stripping without changing its
  // stored title metadata. The upgrade must build the new key from that title.
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord oldRecord = original;
  constexpr char oldKey[] = "iliad";
  oldRecord.foldLen = sizeof(oldKey) - 1;
  std::memset(oldRecord.fold, 0, sizeof(oldRecord.fold));
  std::memcpy(oldRecord.fold, oldKey, oldRecord.foldLen);
  // Old title order is "iliad" before "rendezvous"; the new key puts "the" after it.
  std::memcpy(bytes.data() + recordOffset(header, 0), &oldRecord, sizeof(oldRecord));
  std::memcpy(bytes.data() + recordOffset(header, 1), &other, sizeof(other));
  bytes[offsetof(ClixHeader, foldVersion)] = CLIX_FOLD_VERSION - 1;
  LibraryIndexFile stale;
  ASSERT_TRUE(stale.openForReconciliation(INDEX));
  EXPECT_EQ(pathAt(stale, SortOrder::TitleAsc, 0), "/a.epub");
  stale.close();

  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(after.header().foldVersion, CLIX_FOLD_VERSION);
  EXPECT_EQ(pathAt(after, SortOrder::TitleAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(after, SortOrder::TitleAsc, 1), "/a.epub");
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(std::string(rebuilt.fold, rebuilt.foldLen), "the iliad");
  EXPECT_EQ(foldedGroupInitial(std::string_view(rebuilt.fold, rebuilt.foldLen)), static_cast<uint32_t>('t'));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
}

TEST_F(LibraryBuilderTest, VersionFourIndexKeepsFirstSeenDuringUpgrade) {
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();

  // For two books, the v4 and v6 side arrays fit before the same aligned
  // name section. This models an old index without changing its record data.
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = 4;
  fake::files[INDEX]->bytes[offsetof(ClixHeader, foldVersion)] = 1;
  dropMetadataCache();
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(after.header().formatVersion, CLIX_FORMAT_VERSION);
  EXPECT_EQ(after.header().foldVersion, CLIX_FOLD_VERSION);
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
  EXPECT_EQ(rebuilt.modificationTime, original.modificationTime);
}

TEST_F(LibraryBuilderTest, InterruptedUpgradeRestoresVersionThreeBackup) {
  bookMetadata["/a.epub"].series = "Earthsea";
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();

  constexpr char BACKUP[] = "/.crosspoint/library.bak";
  fake::files[BACKUP] = std::make_shared<fake::Node>(*fake::files[INDEX]);
  fake::files[BACKUP]->bytes[offsetof(ClixHeader, formatVersion)] = 3;
  fake::files[INDEX]->bytes[0] = 'X';  // Damaged live index after install.
  dropMetadataCache();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_FALSE(Storage.exists(BACKUP));
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
}

TEST_F(LibraryBuilderTest, FailedUpgradeKeepsVersionThreeShelfReadable) {
  bookMetadata["/a.epub"].series = "Earthsea";
  initial();
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = 3;
  fake::failWritePath = "/.crosspoint/library.new";

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile shelf;
  EXPECT_FALSE(shelf.open(INDEX));
  ASSERT_TRUE(shelf.openForReconciliation(INDEX));
  EXPECT_EQ(shelf.bookCount(), 2);
  EXPECT_EQ(pathAt(shelf, SortOrder::TitleAsc, 0), "/a.epub");
  ClixRecord record{};
  ASSERT_TRUE(recordAtPath(shelf, "/a.epub", record));
  std::string series;
  ASSERT_TRUE(shelf.readSeries(record, series));
  EXPECT_EQ(series, "Earthsea");
}

TEST_F(LibraryBuilderTest, VersionTwoIndexRebuildKeepsFirstSeenOrder) {
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();

  // The old format has the same header and record stride. Reconciliation only
  // needs those records and path hashes; its shorter metadata blob is replaced.
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = 2;
  dropMetadataCache();
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);

  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(after.header().formatVersion, CLIX_FORMAT_VERSION);
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, ZeroTimestampDoesNotReuseStaleEpubCache) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/a.epub"].title = "Original title";
  initial();

  cachedBookMetadata["/a.epub"].title = "Original title";
  bookMetadata["/a.epub"].title = "Replacement title";
  metadataCacheUse.clear();
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  ASSERT_FALSE(metadataCacheUse.empty());
  EXPECT_FALSE(metadataCacheUse.front());
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", record));
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "Replacement title");
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  // Metadata parsed by the first build survives the round trip through the
  // metadata-off index in the persistent cache.
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataCached, 2);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 1);
  ClixRecord record{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", record));
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "Title");
  index.close();

  dropMetadataCache();
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, SortsPastTheFirstTwelveTitleAndSurnameBytes) {
  const std::string commonPrefix(36, 'Q');
  bookMetadata["/a.epub"].title = commonPrefix + " Z";
  bookMetadata["/b.epub"].title = commonPrefix + " A";
  bookMetadata["/a.epub"].author = "Alice " + commonPrefix + "Z";
  bookMetadata["/b.epub"].author = "Bob " + commonPrefix + "A";

  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 1), "/a.epub");
}

TEST_F(LibraryBuilderTest, FirstNameAndLastNameAuthorOrdersDiffer) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Zoe Adams";
  bookMetadata["/b.epub"].author = "Amy Young";
  bookMetadata["/c.epub"].author = "Beth Moore";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 1), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, 1), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, 2), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstDesc, 0), "/a.epub");
}

TEST_F(LibraryBuilderTest, FirstNameSortRefinesLongSharedPrefixes) {
  const std::string prefix(30, 'Q');
  bookMetadata["/a.epub"].author = prefix + " Zoe";
  bookMetadata["/b.epub"].author = prefix + " Amy";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, 1), "/a.epub");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, DateAddedUsesCreationTimeRatherThanModificationTime) {
  // Modification dates point in the opposite direction. Books with equal
  // creation times retain their first-seen order.
  fake::add("/c.epub", "book c", /*time=*/9);
  fake::add("/d.epub", "book d", /*time=*/0);
  fake::files["/c.epub"]->created = 0;
  fake::files["/d.epub"]->created = 9;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
  uint32_t created = 0;
  EXPECT_TRUE(index.readCreationTime(index.ordinalForRow(SortOrder::RecentAsc, 0), created));
  EXPECT_EQ(created, 0u);
}

TEST_F(LibraryBuilderTest, MissingCreationTimesFallBackToFirstSeenAcrossRebuilds) {
  fake::files["/a.epub"]->created = 0;
  fake::files["/b.epub"]->created = 0;
  initial();
  fake::add("/c.epub", "book c", /*time=*/100);
  fake::files["/c.epub"]->created = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
}

TEST_F(LibraryBuilderTest, CreationTimeChangeUpdatesOrderWithoutReparsingMetadata) {
  initial();
  fake::files["/a.epub"]->created = 2;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  uint32_t created = 0;
  ASSERT_TRUE(index.readCreationTime(index.ordinalForRow(SortOrder::RecentAsc, 1), created));
  EXPECT_EQ(created, 2u);
}

TEST_F(LibraryBuilderTest, EveryAllocationFailureFailsCleanlyOrBuildsTheSameIndex) {
  const auto populate = [] {
    fake::reset();
    for (unsigned i = 0; i < 40; i++) {
      const std::string path = "/shelf/book" + numbered("", (i * 17) % 40) + ".txt";
      fake::add(path, std::string(i + 1, 'x'), 3);
      fake::files[path]->created = (i * 7) % 5;
    }
  };
  populate();
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  const auto reference = fake::files[INDEX]->bytes;

  bool sawFailure = false;
  bool sawRecovery = false;
  for (int failAt = 0; failAt < 64; failAt++) {
    populate();
    fake::failAlloc = failAt;
    const bool built = buildLibraryIndex("/", stats, false);
    const bool injected = fake::failureTriggered;
    fake::failAlloc = -1;
    for (const char* scratch :
         {"/.crosspoint/library.stage", "/.crosspoint/library.order", "/.crosspoint/library.canon",
          "/.crosspoint/library.authors", "/.crosspoint/library.runs", "/.crosspoint/library.prior"}) {
      EXPECT_FALSE(Storage.exists(scratch)) << failAt << ' ' << scratch;
    }
    if (!built) {
      sawFailure = true;
      EXPECT_EQ(stats.failure, BuildFailure::Error) << failAt;
      EXPECT_FALSE(Storage.exists(INDEX)) << failAt;
      EXPECT_TRUE(libraryIndexNeedsRefresh());
      continue;
    }
    // Fallible-but-optional allocations (buffers, smaller sort runs) must not
    // change the result. Duplicate detection and spelling harmonisation may
    // degrade instead, and the index says so and retries.
    if (stats.ranksDegraded || stats.dedupDegraded) {
      LibraryIndexFile index;
      ASSERT_TRUE(index.open(INDEX)) << failAt;
      EXPECT_EQ(index.bookCount(), 40) << failAt;
      EXPECT_TRUE(libraryIndexNeedsRefresh());
    } else {
      EXPECT_EQ(fake::files[INDEX]->bytes, reference) << failAt;
      if (injected) sawRecovery = true;
    }
    if (!injected) break;
  }
  EXPECT_TRUE(sawFailure);
  EXPECT_TRUE(sawRecovery);
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_FALSE(stats.dedupAllocFailed);
  // The cap is a property of the card's layout; rescanning would hit it again.
  EXPECT_FALSE(libraryIndexNeedsRefresh());
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, UnreadableBooksDoNotKeepTheIndexDirty) {
  fake::add("/empty.epub", "");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.unreadableSkipped, 1);
  EXPECT_EQ(stats.books, 2);
  EXPECT_FALSE(libraryIndexNeedsRefresh());
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 3;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndOldCeilingKeepAllOrders) {
  // 4,096 books spill two sort runs on a device without PSRAM.
  for (const unsigned count : {513u, 4096u}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == 4096) {
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, 4096);
      // The fixed-size per-directory duplicate tracker is deliberately bounded
      // below the maximum library size, so this index remains degraded. It must
      // rebuild rather than silently preserve an old degraded header.
      EXPECT_TRUE(stats.indexReplaced);
      EXPECT_TRUE(stats.dedupDegraded);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorFirstAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, BookPastFormatCeilingKeepsPreviousIndex) {
  EXPECT_EQ(libraryBookLimit(), CLIX_MAX_RECORDS);
  initial();
  const auto previous = fake::files[INDEX]->bytes;
  for (unsigned i = 0; i < CLIX_MAX_RECORDS - 1u; i++) {
    fake::add("/shelf" + numbered("", i % 64) + "/book" + numbered("", i) + ".txt");
  }

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.failure, BuildFailure::TooManyBooks);
  EXPECT_EQ(fake::files[INDEX]->bytes, previous);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, FormatCeilingLibraryBuildsWithBoundedSortRuns) {
  fake::reset();
  constexpr unsigned count = CLIX_MAX_RECORDS;
  for (unsigned i = 0; i < count; i++) {
    const unsigned title = (i * 7919u) % count;
    const std::string path = "/shelf" + numbered("", i % 128) + "/book" + numbered("", i) + ".txt";
    fake::add(path, std::string(1 + i % 7, 'x'), 3);
    fake::files[path]->created = 1 + title;
  }
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, count);
  EXPECT_FALSE(stats.ranksDegraded);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), count);
  // Title order is the filename order; arrival order is creation-time order.
  std::string previousTitle;
  for (uint16_t row = 0; row < count; row += 257) {
    const std::string path = pathAt(index, SortOrder::TitleAsc, row);
    const std::string name = path.substr(path.find_last_of('/') + 1);
    EXPECT_LT(previousTitle, name) << row;
    previousTitle = name;
  }
  for (const uint16_t row : {0u, 1u, 4095u, 16383u, 32766u}) {
    const std::string path = pathAt(index, SortOrder::RecentAsc, row);
    ASSERT_FALSE(path.empty()) << row;
    EXPECT_EQ(fake::files[path]->created, 1u + row) << row;
  }
}

TEST_F(LibraryBuilderTest, PriorDedupDegradationForcesReplacement) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.flags |= CLIX_FLAG_DEDUP_DEGRADED;
  std::memcpy(bytes.data(), &header, sizeof(header));

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().flags & CLIX_FLAG_DEDUP_DEGRADED, 0);
}

TEST_F(LibraryBuilderTest, DirtyIndexClearsOnSuccessAndRetriesAfterFailure) {
  EXPECT_TRUE(libraryIndexNeedsRefresh());
  initial();
  EXPECT_FALSE(libraryIndexNeedsRefresh());
  invalidateLibraryIndex();
  fake::failOpenPath = "/.crosspoint/library.idx";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.failure, BuildFailure::Error);
  EXPECT_TRUE(libraryIndexNeedsRefresh());
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(libraryIndexNeedsRefresh());
}

TEST_F(LibraryBuilderTest, InvalidationDuringScanSurvivesSuccessfulPublish) {
  fake::onService = &invalidateLibraryIndex;
  initial();
  EXPECT_GT(fake::delays, 0u);
  EXPECT_TRUE(libraryIndexNeedsRefresh());
  fake::onService = nullptr;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(libraryIndexNeedsRefresh());
}

TEST_F(LibraryBuilderTest, EmptyLibraryStillRecordsMetadataModeChanges) {
  fake::files.erase("/a.epub");
  fake::files.erase("/b.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 1);
}

TEST_F(LibraryBuilderTest, SleepRestorationStillAllowsFileChangesToInvalidate) {
  restoreLibraryIndexAfterSleep();
  EXPECT_FALSE(libraryIndexNeedsRefresh());
  invalidateLibraryIndex();
  EXPECT_TRUE(libraryIndexNeedsRefresh());
  initial();
  EXPECT_FALSE(libraryIndexNeedsRefresh());
}

namespace {

struct BuildProbe {
  // Cancel once this many EPUBs have been parsed; UINT_MAX never cancels.
  unsigned cancelAfterParses = ~0u;
  unsigned progressCalls = 0;
  bool sawOrganizing = false;
  uint16_t booksWhenOrganizing = 0;

  static bool cancel(void* context) { return fake::parses >= static_cast<BuildProbe*>(context)->cancelAfterParses; }
  static void progress(void* context, const BuildProgress& progress) {
    auto* probe = static_cast<BuildProbe*>(context);
    probe->progressCalls++;
    if (progress.phase == BuildPhase::Organizing && !probe->sawOrganizing) {
      probe->sawOrganizing = true;
      probe->booksWhenOrganizing = progress.books;
    }
  }
  BuildCallbacks callbacks() {
    BuildCallbacks callbacks;
    callbacks.context = this;
    callbacks.cancelRequested = &cancel;
    callbacks.progress = &progress;
    return callbacks;
  }
};

}  // namespace

TEST_F(LibraryBuilderTest, CancelledScanKeepsPreviousIndexAndSaysWhy) {
  initial();
  const auto previous = fake::files[INDEX]->bytes;
  for (unsigned i = 0; i < 40; i++) fake::add("/book" + numbered("", i) + ".epub");

  BuildProbe probe;
  probe.cancelAfterParses = 0;
  const BuildCallbacks callbacks = probe.callbacks();
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &callbacks));
  EXPECT_EQ(stats.failure, BuildFailure::Cancelled);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, previous);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.new"));
  EXPECT_TRUE(libraryIndexNeedsRefresh());

  // Cancellation belongs to one build: the next one without callbacks completes.
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.failure, BuildFailure::None);
  EXPECT_EQ(stats.books, 42);
}

TEST_F(LibraryBuilderTest, ProgressReportsBooksFoundAndTheOrganizingPhase) {
  for (unsigned i = 0; i < 40; i++) fake::add("/book" + numbered("", i) + ".txt");
  BuildProbe probe;
  const BuildCallbacks callbacks = probe.callbacks();
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, &callbacks));
  EXPECT_EQ(stats.failure, BuildFailure::None);
  EXPECT_GT(probe.progressCalls, 0u);
  EXPECT_TRUE(probe.sawOrganizing);
  EXPECT_EQ(probe.booksWhenOrganizing, 42);
}

TEST_F(LibraryBuilderTest, CancelledScanKeepsParsedMetadataForTheNextScan) {
  for (unsigned i = 0; i < 40; i++) {
    const std::string path = "/book" + numbered("", i) + ".epub";
    fake::add(path, "book" + numbered("", i), 7);
    bookMetadata[path].title = numbered("Title ", i);
    bookMetadata[path].series = "Series";
    bookMetadata[path].seriesIndex = std::to_string(i);
  }

  BuildProbe probe;
  probe.cancelAfterParses = 5;
  const BuildCallbacks callbacks = probe.callbacks();
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &callbacks));
  EXPECT_EQ(stats.failure, BuildFailure::Cancelled);
  EXPECT_FALSE(Storage.exists(INDEX));
  const unsigned parsedBeforeCancel = fake::parses;
  ASSERT_GE(parsedBeforeCancel, 5u);
  ASSERT_LT(parsedBeforeCancel, 42u);

  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 42u - parsedBeforeCancel);
  EXPECT_EQ(stats.metadataCached, parsedBeforeCancel);
  EXPECT_EQ(stats.parsed, 42u - parsedBeforeCancel);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  for (unsigned i = 0; i < 40; i++) {
    ClixRecord record{};
    const std::string path = "/book" + numbered("", i) + ".epub";
    ASSERT_TRUE(recordAtPath(index, path, record)) << path;
    EXPECT_EQ(record.metadataStatus, CLIX_METADATA_EXTRACTED);
    std::string title;
    std::string series;
    ASSERT_TRUE(index.readTitle(record, title));
    ASSERT_TRUE(index.readSeries(record, series));
    EXPECT_EQ(title, numbered("Title ", i));
    EXPECT_EQ(series, "Series");
  }
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/book0000.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 39), "/book0039.epub");
}

TEST_F(LibraryBuilderTest, CachedMetadataIsIgnoredOnceTheBookChanges) {
  initial();
  // Lose the previous index, so only the cache can supply metadata.
  Storage.remove(INDEX);
  bookMetadata["/a.epub"].title = "Edited";
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataCached, 1);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(recordAtPath(index, "/a.epub", record));
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "Edited");
}

TEST_F(LibraryBuilderTest, DamagedCachedMetadataIsReparsedRatherThanTrusted) {
  initial();
  Storage.remove(INDEX);
  auto& payload = fake::files[LibraryMetadataCache::payloadPath()]->bytes;
  ASSERT_FALSE(payload.empty());
  for (auto& byte : payload) byte ^= 0x5A;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataCached, 0);

  // A cache from another version is discarded wholesale.
  Storage.remove(INDEX);
  fake::files[LibraryMetadataCache::slotPath()]->bytes[4] = 99;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  Storage.remove(INDEX);
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u);
}

TEST_F(LibraryBuilderTest, FailedExtractionIsNotCached) {
  bookMetadata["/a.epub"].success = false;
  initial();
  Storage.remove(INDEX);
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataCached, 1);
}

TEST_F(LibraryBuilderTest, SpilledAndInRamSortsBuildIdenticalIndexes) {
  const auto populate = [] {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < 300; i++) {
      const std::string path = "/b" + numbered("", (i * 37) % 300) + ".epub";
      fake::add(path, std::string(1 + i, 'x'), 5);
      fake::files[path]->created = i % 9;
      auto& metadata = bookMetadata[path];
      // Long shared prefixes force tie refinement across spilled runs.
      metadata.title = "The Collected Stories of " + numbered("Volume ", i % 23);
      metadata.author = i % 11 == 0 ? std::string() : numbered("Ursula Writer ", i % 13);
      if (i % 4 == 0) metadata.author = numbered("Writer, Ursula ", i % 13);
      metadata.series = i % 5 == 0 ? std::string() : "A Very Long Series Name That Ties " + numbered("", i % 3);
      metadata.seriesIndex = std::to_string(i % 17);
      metadata.genre = numbered("Speculative Fiction Genre ", i % 6);
    }
  };
  populate();
  fake::psram = true;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_GT(fake::psramAllocations, 0u);
  const auto inRam = fake::files[INDEX]->bytes;

  // Twenty-entry runs: 15 runs merged one entry per slice is the worst case.
  populate();
  fake::psram = false;
  setSortRunCapacityForTesting(20);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, inRam);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.runs"));

  // A device without PSRAM now indexes past the old 4,096-book limit too.
  EXPECT_EQ(libraryBookLimit(), CLIX_MAX_RECORDS);
}

namespace {

struct SortHarness {
  std::vector<std::string> values;
  unsigned loads = 0;
  static bool load(void* context, const uint16_t source, const size_t offset, char* segment, size_t& valueBytes) {
    static_cast<SortHarness*>(context)->loads++;
    const auto& value = static_cast<SortHarness*>(context)->values[source];
    valueBytes = value.size();
    std::memset(segment, 0, SORT_SEGMENT_BYTES);
    if (offset < value.size())
      std::memcpy(segment, value.data() + offset, std::min(value.size() - offset, SORT_SEGMENT_BYTES));
    return true;
  }
  static bool collect(void* context, const SortEntry& entry) {
    static_cast<std::vector<uint16_t>*>(context)->push_back(entry.ordinal);
    return true;
  }
};

}  // namespace

TEST_F(LibraryBuilderTest, ExternalSorterMatchesAFullSortAtEveryRunSize) {
  std::mt19937 random(7);
  SortHarness harness;
  for (unsigned i = 0; i < 1000; i++) {
    // Values share long prefixes, and some are "unknown" (all-0xFF prefix).
    std::string value = i % 10 == 0 ? std::string() : "shared-prefix-" + std::to_string(random() % 40);
    if (i % 3 == 0) value += "-and-a-much-longer-shared-tail-" + std::to_string(random() % 5);
    harness.values.push_back(value);
  }
  std::vector<uint16_t> expected(harness.values.size());
  std::iota(expected.begin(), expected.end(), 0);
  std::stable_sort(expected.begin(), expected.end(), [&](const uint16_t a, const uint16_t b) {
    const auto& left = harness.values[a];
    const auto& right = harness.values[b];
    if (left.empty() || right.empty()) return !left.empty() && right.empty();
    return left < right;
  });

  for (const uint16_t capacity : {uint16_t{0}, uint16_t{40}, uint16_t{97}, uint16_t{1000}}) {
    setSortRunCapacityForTesting(capacity);
    SortConfig config;
    config.runPath = "/.crosspoint/test.runs";
    config.keyBytes = 72;
    config.load = &SortHarness::load;
    config.loadContext = &harness;
    ExternalSorter sorter;
    ASSERT_TRUE(sorter.begin(config, static_cast<uint16_t>(harness.values.size())));
    // Entries arrive shuffled so runs are not already in order.
    std::vector<uint16_t> arrival(expected);
    std::shuffle(arrival.begin(), arrival.end(), random);
    for (const uint16_t source : arrival) {
      SortEntry entry{};
      if (harness.values[source].empty())
        std::memset(entry.key, 0xFF, sizeof(entry.key));
      else {
        size_t valueBytes = 0;
        SortHarness::load(&harness, source, 0, entry.key, valueBytes);
      }
      entry.ordinal = source;
      entry.source = source;
      ASSERT_TRUE(sorter.add(entry));
    }
    std::vector<uint16_t> sorted;
    ASSERT_TRUE(sorter.finish(&SortHarness::collect, &sorted)) << capacity;
    EXPECT_EQ(sorted, expected) << capacity;
    EXPECT_FALSE(Storage.exists(config.runPath));
  }
}

TEST_F(LibraryBuilderTest, ExternalSorterRetriesWithASmallerBufferAfterAllocationFailure) {
  SortConfig config;
  config.runPath = "/.crosspoint/test.runs";
  ExternalSorter sorter;
  fake::failAlloc = 0;
  ASSERT_TRUE(sorter.begin(config, 3000));
  EXPECT_TRUE(fake::failureTriggered);
  for (uint16_t i = 0; i < 3000; i++) {
    SortEntry entry{};
    entry.key[0] = static_cast<char>(i % 251);
    entry.ordinal = i;
    ASSERT_TRUE(sorter.add(entry));
  }
  EXPECT_TRUE(sorter.spilled());
  std::vector<uint16_t> sorted;
  ASSERT_TRUE(sorter.finish(&SortHarness::collect, &sorted));
  ASSERT_EQ(sorted.size(), 3000u);
  for (size_t i = 1; i < sorted.size(); i++) {
    const auto key = [](const uint16_t ordinal) { return std::make_pair(ordinal % 251, ordinal); };
    EXPECT_LT(key(sorted[i - 1]), key(sorted[i]));
  }
}

TEST_F(LibraryBuilderTest, SpilledMergeOfEqualLongKeysLoadsEachEntryOnlyAFewTimes) {
  // A Calibre library where every book shares one genre: equal values must
  // stop refining at the end of the value, not at the 517-byte key limit.
  setSortRunCapacityForTesting(256);
  SortHarness harness;
  harness.values.assign(3000, "fiction");
  SortConfig config;
  config.runPath = "/.crosspoint/test.runs";
  config.keyBytes = 517;
  config.load = &SortHarness::load;
  config.loadContext = &harness;
  ExternalSorter sorter;
  ASSERT_TRUE(sorter.begin(config, 3000));
  for (uint16_t i = 0; i < 3000; i++) {
    SortEntry entry{};
    size_t valueBytes = 0;
    SortHarness::load(&harness, i, 0, entry.key, valueBytes);
    entry.ordinal = static_cast<uint16_t>(2999 - i);
    entry.source = i;
    ASSERT_TRUE(sorter.add(entry));
  }
  harness.loads = 0;
  std::vector<uint16_t> sorted;
  ASSERT_TRUE(sorter.finish(&SortHarness::collect, &sorted));
  ASSERT_EQ(sorted.size(), 3000u);
  for (uint16_t i = 0; i < 3000; i++) EXPECT_EQ(sorted[i], i);
  EXPECT_LT(harness.loads, 3u * 3000u);
}

TEST_F(LibraryBuilderTest, SpilledMergeOfEqualValuesLongerThanTwoSegmentsStaysBounded) {
  // BISAC-style subjects and long series names tie well past 24 bytes.
  setSortRunCapacityForTesting(256);
  SortHarness harness;
  harness.values.assign(3000, "fiction / science fiction / space opera / general");
  SortConfig config;
  config.runPath = "/.crosspoint/test.runs";
  config.keyBytes = 517;
  config.load = &SortHarness::load;
  config.loadContext = &harness;
  ExternalSorter sorter;
  ASSERT_TRUE(sorter.begin(config, 3000));
  for (uint16_t i = 0; i < 3000; i++) {
    SortEntry entry{};
    size_t valueBytes = 0;
    SortHarness::load(&harness, i, 0, entry.key, valueBytes);
    entry.ordinal = static_cast<uint16_t>(2999 - i);
    entry.source = i;
    ASSERT_TRUE(sorter.add(entry));
  }
  harness.loads = 0;
  std::vector<uint16_t> sorted;
  ASSERT_TRUE(sorter.finish(&SortHarness::collect, &sorted));
  ASSERT_EQ(sorted.size(), 3000u);
  for (uint16_t i = 0; i < 3000; i++) EXPECT_EQ(sorted[i], i);
  // Four segments past the prefix, loaded once per entry in its run and once
  // more as a merge head; never once per comparison.
  EXPECT_LT(harness.loads, 10u * 3000u);
}

TEST_F(LibraryBuilderTest, CancellingWhileCreatingTheMetadataCacheLeavesNoPartialTable) {
  for (unsigned i = 0; i < 4; i++) fake::add("/book" + numbered("", i) + ".epub");
  BuildProbe probe;
  probe.cancelAfterParses = 1;
  const BuildCallbacks callbacks = probe.callbacks();
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &callbacks));
  EXPECT_EQ(stats.failure, BuildFailure::Cancelled);
  // Either the table was completed before the cancel landed, or it is gone.
  if (Storage.exists(LibraryMetadataCache::slotPath())) {
    EXPECT_EQ(fake::files[LibraryMetadataCache::slotPath()]->bytes.size(), 512u + 65536u * 16u);
  }
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 6);
}

TEST_F(LibraryBuilderTest, VersionSixSeriesMetadataIsReparsedIncludingPersistentCache) {
  bookMetadata["/a.epub"].series = "Old wrong series";
  initial();
  LibraryIndexFile before;
  ASSERT_TRUE(before.open(INDEX));
  ClixRecord original{};
  ASSERT_TRUE(recordAtPath(before, "/a.epub", original));
  before.close();
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = 6;
  ASSERT_TRUE(fake::files.count(library::LibraryMetadataCache::slotPath()));
  fake::files[library::LibraryMetadataCache::slotPath()]->bytes[4] = 1;
  bookMetadata["/a.epub"].series = "Correct series";
  bookMetadata["/a.epub"].seriesIndex = "2.5";
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  LibraryIndexFile after;
  ASSERT_TRUE(after.open(INDEX));
  EXPECT_EQ(after.header().formatVersion, CLIX_FORMAT_VERSION);
  ClixRecord rebuilt{};
  ASSERT_TRUE(recordAtPath(after, "/a.epub", rebuilt));
  EXPECT_EQ(rebuilt.firstSeen, original.firstSeen);
  std::string series;
  ASSERT_TRUE(after.readSeries(rebuilt, series));
  EXPECT_EQ(series, "Correct series");
}
