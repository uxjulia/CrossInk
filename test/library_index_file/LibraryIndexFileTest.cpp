#include <Memory.h>
#include <gtest/gtest.h>

#include <cstring>
#include <utility>
#include <vector>

#include "LibraryIndexFile.h"

namespace library {

std::string joinLibraryPath(const std::string_view folder, const std::string_view name) {
  return std::string(folder) + "/" + std::string(name);
}

}  // namespace library

namespace {

std::vector<uint8_t> makeBlob(const uint64_t pathHash, const std::initializer_list<uint8_t> fields) {
  std::vector<uint8_t> blob(sizeof(pathHash) + fields.size());
  std::memcpy(blob.data(), &pathHash, sizeof(pathHash));
  std::copy(fields.begin(), fields.end(), blob.begin() + sizeof(pathHash));
  return blob;
}

}  // namespace

TEST(LibraryIndexFile, MissingIndexDoesNotCloseAnUninitializedHandle) {
  Storage.clearFile();
  HalFile::resetInvalidCloseCount();

  {
    library::LibraryIndexFile index;
    EXPECT_FALSE(index.open("/missing.clx"));
  }

  EXPECT_EQ(HalFile::invalidCloseCount(), 0);
}

TEST(LibraryIndexFile, ReadsEveryStoredOrderInBothDirections) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  library::layoutSections(header, 0, 0);

  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t authorOrder[] = {2, 0, 1};
  const uint16_t firstNameOrder[] = {0, 2, 1};
  const uint16_t arrivalOrder[] = {1, 2, 0};
  const uint16_t seriesOrder[] = {1, 0, 2};
  const uint16_t genreOrder[] = {2, 1, 0};
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), authorOrder, sizeof(authorOrder));
  std::memcpy(bytes.data() + library::firstNameOrderOffset(header, 0), firstNameOrder, sizeof(firstNameOrder));
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  std::memcpy(bytes.data() + library::seriesOrderOffset(header, 0), seriesOrder, sizeof(seriesOrder));
  std::memcpy(bytes.data() + library::genreOrderOffset(header, 0), genreOrder, sizeof(genreOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));

  const auto expectOrder = [&](const library::SortOrder order, const uint16_t a, const uint16_t b, const uint16_t c) {
    EXPECT_EQ(index.ordinalForRow(order, 0), a);
    EXPECT_EQ(index.ordinalForRow(order, 1), b);
    EXPECT_EQ(index.ordinalForRow(order, 2), c);
    EXPECT_EQ(index.ordinalForRow(order, 3), 0xFFFF);
  };
  expectOrder(library::SortOrder::RecentAsc, 1, 2, 0);
  expectOrder(library::SortOrder::RecentDesc, 0, 2, 1);
  expectOrder(library::SortOrder::TitleAsc, 0, 1, 2);
  expectOrder(library::SortOrder::TitleDesc, 2, 1, 0);
  expectOrder(library::SortOrder::AuthorAsc, 2, 0, 1);
  expectOrder(library::SortOrder::AuthorDesc, 1, 0, 2);
  expectOrder(library::SortOrder::AuthorFirstAsc, 0, 2, 1);
  expectOrder(library::SortOrder::AuthorFirstDesc, 1, 2, 0);
  expectOrder(library::SortOrder::SeriesAsc, 1, 0, 2);
  expectOrder(library::SortOrder::SeriesDesc, 2, 0, 1);
  expectOrder(library::SortOrder::GenreAsc, 2, 1, 0);
  expectOrder(library::SortOrder::GenreDesc, 0, 1, 2);
}

TEST(LibraryIndexFile, ResolvesRecentRowsByIdentity) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  // One 8-byte path hash blob per record.
  library::layoutSections(header, 0, 3 * sizeof(uint64_t));
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));

  // Ordinals 0 and 2 share a size, so only the hash can tell them apart.
  constexpr uint64_t HASHES[] = {11, 22, 33};
  constexpr uint32_t SIZES[] = {100, 200, 100};
  for (uint16_t ordinal = 0; ordinal < 3; ordinal++) {
    library::ClixRecord record{};
    record.fileSize = SIZES[ordinal];
    record.nameOff = ordinal * sizeof(uint64_t);
    std::memcpy(bytes.data() + library::recordOffset(header, ordinal), &record, sizeof(record));
    std::memcpy(bytes.data() + header.nameStart + record.nameOff, &HASHES[ordinal], sizeof(uint64_t));
  }
  const uint16_t arrivalOrder[] = {1, 2, 0};
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  const library::BookIdentity books[] = {
      {HASHES[0], SIZES[0]},  // ordinal 0 -> ascending row 2
      {HASHES[2], SIZES[2]},  // same size as ordinal 0, hash picks ordinal 2 -> row 1
      {99, SIZES[0]},         // size matches, hash does not: absent
      {HASHES[1], 999},       // hash matches, size does not: absent
      {HASHES[1], 0},         // size unknown: the hash alone matches -> row 0
  };
  uint16_t rows[5] = {};
  ASSERT_TRUE(index.recentRowsFor(books, 5, rows));
  EXPECT_EQ(rows[0], 2);
  EXPECT_EQ(rows[1], 1);
  EXPECT_EQ(rows[2], 0xFFFF);
  EXPECT_EQ(rows[3], 0xFFFF);
  EXPECT_EQ(rows[4], 0);
}

TEST(LibraryIndexFile, RejectsInvalidPermutationOrdinal) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, 0);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t invalid = 1;
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), &invalid, sizeof(invalid));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::AuthorAsc, 0), 0xFFFF);
}

TEST(LibraryIndexFile, ReadsPathHashAndEveryPublicBlobField) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {6, '/', 'b', 'o', 'o', 'k', 's'};
  constexpr uint64_t PATH_HASH = 0x0123456789ABCDEFULL;
  const auto blob = makeBlob(PATH_HASH, {'x', 1, 'a', 1, 't', 8, 'O', 'r', 'i', 'g', 'i', 'n', 'a', 'l'});
  header.folderCount = 1;
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint64_t pathHash = 0;
  ASSERT_TRUE(index.readPathHash(record, pathHash));
  EXPECT_EQ(pathHash, PATH_HASH);
  std::string name;
  ASSERT_TRUE(index.readName(record, name));
  EXPECT_EQ(name, "x");
  std::string author;
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "a");
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "t");
  ASSERT_TRUE(index.readSourceAuthor(record, author));
  EXPECT_EQ(author, "Original");
  std::string path;
  ASSERT_TRUE(index.readPath(record, path));
  EXPECT_EQ(path, "/books/x");
  index.close();

  bytes[header.nameStart + sizeof(PATH_HASH) + 5] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readSourceAuthor(record, author));
}

TEST(LibraryIndexFile, RejectsTruncatedAndOverflowingPathHashes) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, sizeof(uint64_t) - 1);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::ClixRecord record{};
  uint64_t hash = 1;
  EXPECT_FALSE(index.readPathHash(record, hash));
  EXPECT_EQ(hash, 0u);
  EXPECT_TRUE(index.ioFailed());

  record.nameOff = UINT32_MAX;
  EXPECT_FALSE(index.readPathHash(record, hash));
}

TEST(LibraryIndexFile, RejectsFolderRecordBeyondFolderBlob) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {5, '/'};
  const auto blob = makeBlob(1, {'x', 0, 0, 0});
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string path;
  EXPECT_FALSE(index.readPath(record, path));
}

TEST(LibraryIndexFile, FolderCheckpointsResolvePathsAcrossBuckets) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  header.folderCount = 130;
  std::vector<uint8_t> folders;
  for (unsigned i = 0; i < header.folderCount; ++i) {
    const std::string dir = "/b" + std::to_string(i);
    folders.push_back(static_cast<uint8_t>(dir.size()));
    folders.insert(folders.end(), dir.begin(), dir.end());
  }
  const auto blob = makeBlob(1, {'x'});
  library::layoutSections(header, folders.size(), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folders.data(), folders.size());
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint32_t offsets[library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT]{};
  uint16_t stride = 0;
  ASSERT_TRUE(index.buildFolderCheckpoints(offsets, stride));
  EXPECT_EQ(stride, 2);
  library::ClixRecord record{};
  record.nameLen = 1;
  std::string path;
  for (const uint16_t folderId : {uint16_t{0}, uint16_t{1}, uint16_t{128}, uint16_t{129}}) {
    record.folderId = folderId;
    ASSERT_TRUE(index.readPath(record, path, offsets, stride));
    EXPECT_EQ(path, "/b" + std::to_string(folderId) + "/x");
  }
}

TEST(LibraryIndexFile, DisplayTextAcceptsMissingMetadataAndRejectsMalformedFields) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const auto blob = makeBlob(123, {'n', 'o', 't', 'e', '.', 't', 'x', 't', 0, 0, 0});
  library::layoutSections(header, 0, blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 8;
  Storage.setFile("/library.clx", bytes);
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title = "previous title";
  std::string author = "previous author";
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_EQ(title, "note");
  EXPECT_TRUE(author.empty());
  index.close();
  bytes[header.nameStart + sizeof(uint64_t) + record.nameLen] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readDisplayText(record, title, author));
}

TEST(LibraryIndexFile, BrowserRowDecodesOnceAndInvalidatesOnReopen) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  auto blob = makeBlob(123, {'x', 1, 'a', 1, 't', 0, 1, 's', 1, 'g'});
  library::layoutSections(header, 0, blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  std::memcpy(bytes.data() + library::recordOffset(header, 0), &record, sizeof(record));
  Storage.setFile("/row.clx", bytes);
  library::LibraryIndexFile index(true);
  ASSERT_TRUE(index.open("/row.clx"));
  const auto before = HalFile::reads;
  ASSERT_TRUE(index.readRecord(0, record));
  std::string title, author, field;
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_EQ(title, "t");
  EXPECT_EQ(author, "a");
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readName(record, field));
  EXPECT_EQ(field, "x");
  ASSERT_TRUE(index.readSeries(record, field));
  EXPECT_EQ(field, "s");
  ASSERT_TRUE(index.readGenre(record, field));
  EXPECT_EQ(field, "g");
  uint64_t hash = 0;
  ASSERT_TRUE(index.readPathHash(record, hash));
  EXPECT_EQ(hash, 123u);
  EXPECT_EQ(HalFile::reads - before, 2u);  // One record read, one blob read.
  index.close();
  ASSERT_TRUE(index.open("/row.clx"));
  failNextObjectAllocation = true;
  const auto allocations = objectAllocationAttempts;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  ASSERT_TRUE(index.readSeries(record, field));
  EXPECT_EQ(title, "t");
  EXPECT_EQ(author, "a");
  EXPECT_EQ(field, "s");
  EXPECT_EQ(objectAllocationAttempts - allocations, 1u);  // No allocation retry loop under pressure.
  index.close();
  bytes[header.nameStart + 12] = 'u';
  Storage.setFile("/row.clx", bytes);
  ASSERT_TRUE(index.open("/row.clx"));
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_EQ(title, "u");
  index.close();
  bytes[header.nameStart + 14] = 255;  // Invalid series length must not leak cached strings.
  Storage.setFile("/row.clx", bytes);
  ASSERT_TRUE(index.open("/row.clx"));
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_FALSE(index.readSeries(record, field));
  EXPECT_TRUE(field.empty());
}

TEST(LibraryIndexFile, BrowserCacheHandlesMaximumFieldsAndSwitchingRows) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 2;
  std::vector<uint8_t> blob;
  std::vector<library::ClixRecord> records(2);
  for (size_t row = 0; row < records.size(); ++row) {
    records[row].nameOff = blob.size();
    records[row].nameLen = 255;
    blob.insert(blob.end(), 8, 0);
    blob.insert(blob.end(), 255, 'n' + row);
    for (uint8_t field = 0; field < 5; ++field) {
      blob.push_back(255);
      blob.insert(blob.end(), 255, 'a' + field + row);
    }
    blob.insert(blob.end(), 4, 0);  // Series position.
  }
  library::layoutSections(header, 0, blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  std::memcpy(bytes.data() + library::recordOffset(header, 0), records.data(), records.size() * sizeof(records[0]));
  Storage.setFile("/max.clx", std::move(bytes));
  library::LibraryIndexFile index(true);
  ASSERT_TRUE(index.open("/max.clx"));
  for (uint16_t row : {0, 1, 0}) {
    library::ClixRecord record{};
    ASSERT_TRUE(index.readRecord(row, record));
    std::string title, author, field;
    ASSERT_TRUE(index.readDisplayText(record, title, author));
    EXPECT_EQ(title, std::string(255, 'b' + row));
    EXPECT_EQ(author, std::string(255, 'a' + row));
    ASSERT_TRUE(index.readSourceAuthor(record, field));
    EXPECT_EQ(field, std::string(255, 'c' + row));
    ASSERT_TRUE(index.readSeries(record, field));
    EXPECT_EQ(field, std::string(255, 'd' + row));
    ASSERT_TRUE(index.readGenre(record, field));
    EXPECT_EQ(field, std::string(255, 'e' + row));
    ASSERT_TRUE(index.readName(record, field));
    EXPECT_EQ(field, std::string(255, 'n' + row));
  }
}
