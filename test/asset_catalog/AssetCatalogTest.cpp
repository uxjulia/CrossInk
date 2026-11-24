#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "AssetCatalog.h"

namespace {
using namespace AssetCatalog;
const std::string hash(64, 'a');
const std::string oldHash(64, 'b');
const std::string revision(40, 'c');
bool supported(const char*, void*) { return true; }
Options translationOptions() {
  Options result;
  result.expectedKind = Kind::Translation;
  result.expectedSource = hash.c_str();
  result.expectedKeyset = oldHash.c_str();
  result.supportedTranslation = supported;
  return result;
}
std::string entry(const std::string& code = "fr", bool translation = false) {
  return "{\"code\":\"" + code + "\",\"bytes\":1,\"sha256\":\"" + hash + "\"" +
         (translation ? ",\"name\":\"French\"" : "") + "}";
}
std::string catalog(const std::string& entries = entry(), bool translation = false) {
  return "{\"schema\":1,\"kind\":\"" + std::string(translation ? "translation" : "hyphenation") +
         "\",\"channel\":\"stable\",\"revision\":\"" + revision +
         "\",\"format\":1,\"requires\":0,\"notices\":\"notices.json\",\"sources\":\"sources.json\"" +
         (translation ? ",\"source\":\"" + hash + "\",\"keyset\":\"" + oldHash + "\"" : "") + ",\"entries\":[" +
         entries + "]}";
}
std::string replace(std::string input, const std::string& from, const std::string& to) {
  const auto found = input.find(from);
  EXPECT_NE(found, std::string::npos);
  if (found != std::string::npos) input.replace(found, from.size(), to);
  return input;
}
bool parse(const std::string& input, const Options& options = {}, size_t chunk = 1) {
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, options);
  for (size_t i = 0; i < input.size(); i += chunk) {
    if (!parser.feed(input.data() + i, std::min(chunk, input.size() - i))) return false;
  }
  return parser.finish();
}
std::string fixture(const char* name) {
  std::ifstream file(std::string(ASSET_FIXTURE_DIR) + "/" + name);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

TEST(AssetCatalog, SelectedAssetKeepsContractsAfterCatalogRelease) {
  auto full = std::make_unique<Catalog>();
  Selection selected;
  {
    Parser parser(*full, translationOptions());
    const auto json = catalog(entry("fr", true), true);
    ASSERT_TRUE(parser.feed(json.data(), json.size()));
    ASSERT_TRUE(parser.finish());
    static_cast<Metadata&>(selected) = *full;
    selected.entry = full->entries[0];
  }
  full.reset();
  EXPECT_EQ(selected.kind, Kind::Translation);
  EXPECT_EQ(selected.channel, Channel::Stable);
  EXPECT_STREQ(selected.source, hash.c_str());
  EXPECT_STREQ(selected.keyset, oldHash.c_str());
  EXPECT_STREQ(selected.entry.code, "fr");
  EXPECT_STREQ(selected.entry.name, "French");
  char path[MAX_CATALOG_PATH];
  ASSERT_TRUE(immutableAssetPath(selected, selected.entry, path, sizeof(path)));
  EXPECT_EQ(std::string(path), "snapshots/" + revision + "/translation/" + hash + ".yaml");
}

TEST(AssetCatalog, DiscoveryPathsKeepContractsAndChannelsSeparate) {
  char path[MAX_CATALOG_PATH];
  auto options = translationOptions();
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_EQ(std::string(path), "catalogs/stable/translation/" + hash + "/" + oldHash + ".json");
  const std::string original(path);
  options.expectedSource = oldHash.c_str();
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_NE(std::string(path), original);
  options.expectedSource = hash.c_str();
  options.expectedKeyset = hash.c_str();
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_NE(std::string(path), original);
  options = translationOptions();
  options.expectedChannel = Channel::Rc;
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_EQ(std::string(path), "catalogs/rc/translation/" + hash + "/" + oldHash + ".json");
  options.expectedKind = Kind::Hyphenation;
  options.expectedSource = options.expectedKeyset = nullptr;
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_STREQ(path, "catalogs/rc/hyphenation.json");
  options.expectedChannel = Channel::Stable;
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_STREQ(path, "catalogs/stable/hyphenation.json");
}
TEST(AssetCatalog, SupplementalHyphenationHasIndependentDiscoveryAndAllowlist) {
  Options options;
  options.hyphenationCatalog = HyphenationCatalog::Supplemental;
  options.supportedSupplementalHyphenation = [](const char* code) { return std::strcmp(code, "be") == 0; };
  char path[MAX_CATALOG_PATH];
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  EXPECT_STREQ(path, "catalogs/stable/hyphenation-supplement.json");
  EXPECT_TRUE(parse(replace(catalog(entry("be")), "\"schema\":1", "\"schema\":3"), options));
  EXPECT_FALSE(parse(replace(catalog(entry("de")), "\"schema\":1", "\"schema\":3"), options));
  EXPECT_FALSE(parse(replace(catalog(entry("nl")), "\"schema\":1", "\"schema\":3"), options));
  EXPECT_FALSE(parse(catalog(entry("be")), options));
  EXPECT_FALSE(canFallbackHyphenationCatalog(options, 404, "de"));
}

TEST(AssetCatalog, ExtendedHyphenationCatalogPreservesLegacyContract) {
  auto legacy = fixture("hyphenation.json");
  ASSERT_FALSE(legacy.empty());
  auto full = std::make_unique<Catalog>();
  Parser legacyParser(*full, {});
  ASSERT_TRUE(legacyParser.feed(legacy.data(), legacy.size()));
  ASSERT_TRUE(legacyParser.finish());
  EXPECT_EQ(full->count, MAX_LEGACY_HYPHENATION_ENTRIES);
  EXPECT_STREQ(full->entries[0].code, "de");
  char path[MAX_CATALOG_PATH];
  ASSERT_TRUE(immutableAssetPath(*full, full->entries[0], path, sizeof(path)));
  EXPECT_EQ(std::string(path),
            "snapshots/" + std::string(full->revision) + "/hyphenation/" + full->entries[0].sha256 + ".cphyph");

  Options extended;
  extended.hyphenationCatalog = HyphenationCatalog::Extended;
  ASSERT_TRUE(catalogPath(extended, path, sizeof(path)));
  EXPECT_STREQ(path, "catalogs/stable/hyphenation-v2.json");
  extended.expectedChannel = Channel::Rc;
  ASSERT_TRUE(catalogPath(extended, path, sizeof(path)));
  EXPECT_STREQ(path, "catalogs/rc/hyphenation-v2.json");
  extended.expectedChannel = Channel::Stable;

  auto modern = replace(legacy, "\"schema\":1", "\"schema\":2");
  modern = replace(modern, "}]}", "}," + entry("nl") + "]}");
  EXPECT_TRUE(parse(modern, extended));
  EXPECT_FALSE(parse(modern));
  EXPECT_FALSE(parse(legacy, extended));
  EXPECT_FALSE(parse(replace(modern, "\"nl\"", "\"da\""), extended));
  EXPECT_FALSE(parse(replace(modern, "}]}", "}," + entry("nl") + "]}"), extended));
  EXPECT_FALSE(parse(replace(catalog(entry("nl")), "\"schema\":1", "\"schema\":2")));
  EXPECT_FALSE(parse(catalog(entry("nl"))));
}

TEST(AssetCatalog, OnlyAbsentExtendedCatalogForLegacyPacksAllowsFallback) {
  Options modern;
  modern.hyphenationCatalog = HyphenationCatalog::Extended;
  EXPECT_TRUE(canFallbackHyphenationCatalog(modern, 404, "de"));
  EXPECT_TRUE(canFallbackHyphenationCatalog(modern, 403, "de"));
  EXPECT_FALSE(canFallbackHyphenationCatalog(modern, 403, "nl"));
  EXPECT_FALSE(canFallbackHyphenationCatalog(modern, 404, "nl"));
  EXPECT_FALSE(canFallbackHyphenationCatalog(modern, 404, nullptr));
  EXPECT_FALSE(canFallbackHyphenationCatalog(modern, 404, "en"));
  for (const int error : {0, 200, 401, 500}) {
    EXPECT_FALSE(canFallbackHyphenationCatalog(modern, error, "de"));
  }
  EXPECT_FALSE(canFallbackHyphenationCatalog({}, 404, "de"));
  modern.expectedKind = Kind::Translation;
  EXPECT_FALSE(canFallbackHyphenationCatalog(modern, 404, "de"));
}

TEST(AssetCatalog, DiscoveryPathsRejectInvalidContractsAndShortBuffers) {
  char path[MAX_CATALOG_PATH];
  auto options = translationOptions();
  ASSERT_TRUE(catalogPath(options, path, sizeof(path)));
  const size_t length = std::strlen(path);
  EXPECT_FALSE(catalogPath(options, path, length));
  EXPECT_STREQ(path, "");
  EXPECT_TRUE(catalogPath(options, path, length + 1));
  EXPECT_FALSE(catalogPath(options, nullptr, sizeof(path)));
  EXPECT_FALSE(catalogPath(options, path, 0));
  for (const char* bad : {static_cast<const char*>(nullptr), "", "../invalid", "abcd"}) {
    options = translationOptions();
    options.expectedSource = bad;
    EXPECT_FALSE(catalogPath(options, path, sizeof(path)));
    EXPECT_STREQ(path, "");
    options = translationOptions();
    options.expectedKeyset = bad;
    EXPECT_FALSE(catalogPath(options, path, sizeof(path)));
  }
  const std::string uppercase(64, 'A');
  options = translationOptions();
  options.expectedSource = uppercase.c_str();
  EXPECT_FALSE(catalogPath(options, path, sizeof(path)));
}

TEST(AssetCatalog, ValidBytewiseAndEveryChunkBoundary) {
  const auto input = catalog();
  for (size_t chunk = 1; chunk <= input.size(); ++chunk) EXPECT_TRUE(parse(input, {}, chunk)) << chunk;
  EXPECT_TRUE(parse(catalog("")));
  EXPECT_TRUE(parse(catalog(entry("FR", true), true), translationOptions()));
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, {});
  ASSERT_TRUE(parser.feed(input.data(), input.size()));
  ASSERT_TRUE(parser.finish());
  EXPECT_TRUE(parser.finish());
  EXPECT_EQ(output->count, 1u);
  EXPECT_STREQ(output->entries[0].code, "fr");
  EXPECT_EQ(output->entries[0].bytes, 1u);
  EXPECT_STREQ(output->revision, revision.c_str());
  EXPECT_EQ(parser.receivedBytes(), input.size());
  EXPECT_FALSE(parser.feed("", 0));
}
TEST(AssetCatalog, ProducerFixtures) {
  const auto hyphenation = fixture("hyphenation.json");
  ASSERT_FALSE(hyphenation.empty());
  EXPECT_TRUE(parse(hyphenation));
  const auto translation = fixture("translation.json");
  ASSERT_FALSE(translation.empty());
  auto options = translationOptions();
  const auto sourceAt = translation.find("\"source\":\"");
  const auto keysetAt = translation.find("\"keyset\":\"");
  ASSERT_NE(sourceAt, std::string::npos);
  ASSERT_NE(keysetAt, std::string::npos);
  const auto source = translation.substr(sourceAt + 10, 64);
  const auto keyset = translation.substr(keysetAt + 10, 64);
  options.expectedSource = source.c_str();
  options.expectedKeyset = keyset.c_str();
  EXPECT_TRUE(parse(translation, options));
}
TEST(AssetCatalog, FieldOrderIsIndependent) {
  auto input = catalog();
  const auto at = input.find(",\"entries\":");
  input = "{" + input.substr(at + 1, input.size() - at - 2) + "," + input.substr(1, at - 1) + "}";
  EXPECT_TRUE(parse(input));
  EXPECT_TRUE(parse(catalog("{\"sha256\":\"" + hash + "\",\"bytes\":262144,\"code\":\"fr\"}")));
}
TEST(AssetCatalog, TruncationAtEveryByteFailsAndErrorIsSticky) {
  const auto input = catalog();
  for (size_t i = 0; i < input.size(); ++i) EXPECT_FALSE(parse(input.substr(0, i))) << i;
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, {});
  EXPECT_FALSE(parser.feed("!", 1));
  const char* error = parser.error();
  EXPECT_FALSE(parser.feed(input.data(), input.size()));
  EXPECT_FALSE(parser.finish());
  EXPECT_EQ(error, parser.error());
}
TEST(AssetCatalog, MalformedJsonAndWrongTypesFail) {
  const auto input = catalog();
  for (const auto* value : {"null", "true", "false", "[]", "{}", "\"1\"", "-1", "1.0", "1e0", "01", "+1", "1 1"})
    EXPECT_FALSE(parse(replace(input, "\"schema\":1", std::string("\"schema\":") + value))) << value;
  for (const auto* value : {"null", "true", "0", "{}", "\"\""})
    EXPECT_FALSE(parse(replace(input, "[" + entry() + "]", value))) << value;
  EXPECT_FALSE(parse(input + "{}"));
  EXPECT_FALSE(parse(input + "x"));
  EXPECT_FALSE(parse(replace(input, "]}", ",]}")));
  EXPECT_FALSE(parse(replace(input, "]}", "],}")));
  EXPECT_FALSE(parse(replace(input, "\"bytes\":1,", "\"bytes\":1,,")));
  EXPECT_FALSE(parse(replace(input, "\"bytes\":1", "\"bytes\" 1")));
  EXPECT_FALSE(parse(replace(input, "\"bytes\":1", "\"bytes\":1e2")));
  EXPECT_FALSE(parse(replace(input, "\"fr\"", "[\"fr\"]")));
  EXPECT_FALSE(parse(replace(input, "\"fr\"", "{\"a\":{\"b\":{\"c\":1}}}")));
  EXPECT_FALSE(parse("\xef\xbb\xbf" + input));
  EXPECT_FALSE(parse(std::string(10000, '[')));
}
TEST(AssetCatalog, RequiredDuplicateUnknownAndForbiddenFields) {
  const auto input = catalog();
  for (const auto* key :
       {"schema", "kind", "channel", "revision", "format", "requires", "notices", "sources", "entries"}) {
    const auto start = input.find(std::string("\"") + key + "\":");
    const auto end = input.find(',', start);
    auto removed = input;
    if (std::string(key) == "entries")
      removed.erase(start - 1, input.size() - start);
    else
      removed.erase(start, end - start + 1);
    EXPECT_FALSE(parse(removed)) << key;
  }
  for (const auto* field : {"\"schema\":1", "\"sc\\u0068ema\":1", "\"extra\":1", "\"source\":\"x\"", "\"keyset\":\"x\"",
                            "\"01234567890123456789012345678901\":1"})
    EXPECT_FALSE(parse("{" + std::string(field) + "," + input.substr(1))) << field;
  for (const auto* field :
       {"\"code\":\"fr\"", "\"bytes\":1", "\"sha256\":\"x\"", "\"name\":\"French\"", "\"url\":\"x\""})
    EXPECT_FALSE(parse(catalog("{" + std::string(field) + "," + entry().substr(1)))) << field;
  EXPECT_FALSE(parse(catalog("{}")));
  EXPECT_FALSE(parse(catalog(entry("FR"), true), translationOptions()));
  EXPECT_FALSE(
      parse(replace(catalog(entry("FR", true), true), ",\"keyset\":\"" + oldHash + "\"", ""), translationOptions()));
}
TEST(AssetCatalog, CompatibilityAndRegistryFailClosed) {
  const auto input = catalog();
  for (const auto* field : {"schema", "format"})
    EXPECT_FALSE(parse(replace(input, std::string("\"") + field + "\":1", std::string("\"") + field + "\":2")));
  EXPECT_FALSE(parse(replace(input, "\"requires\":0", "\"requires\":1")));
  EXPECT_FALSE(parse(replace(input, "stable", "rc")));
  Options rc;
  rc.expectedChannel = Channel::Rc;
  EXPECT_TRUE(parse(replace(input, "stable", "rc"), rc));
  EXPECT_FALSE(parse(replace(input, "hyphenation", "translation")));
  auto options = translationOptions();
  const auto translation = catalog(entry("FR", true), true);
  EXPECT_FALSE(parse(replace(translation, "\"source\":\"" + hash, "\"source\":\"" + oldHash), options));
  options.supportedTranslation = [](const char*, void*) { return false; };
  EXPECT_TRUE(parse(translation, options));
  options.supportedTranslation = nullptr;
  EXPECT_FALSE(parse(translation, options));
  options = translationOptions();
  options.expectedSource = nullptr;
  EXPECT_FALSE(parse(translation, options));
  for (const auto* code :
       {"en", "EN", "Fr", "nl", "", "../fr", "a/b", "1fr", "fr.fr", "fr%2f", "é", "abcdefghijklmnopqrstuvwxyzabcdef"})
    EXPECT_FALSE(parse(catalog(entry(code)))) << code;
  EXPECT_FALSE(parse(catalog(entry("EN", true), true), translationOptions()));
}
TEST(AssetCatalog, FutureTranslationsAreValidatedBeforeFiltering) {
  auto options = translationOptions();
  options.supportedTranslation = [](const char* code, void*) { return std::strcmp(code, "FR") == 0; };
  const auto input = catalog(entry("NEW", true) + ',' + entry("FR", true) + ',' + entry("OTHER", true), true);
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, options);
  ASSERT_TRUE(parser.feed(input.data(), input.size()));
  ASSERT_TRUE(parser.finish());
  ASSERT_EQ(output->count, 1u);
  EXPECT_STREQ(output->entries[0].code, "FR");
  EXPECT_STREQ(output->entries[0].sha256, hash.c_str());
  EXPECT_TRUE(parse(catalog(entry("NEW", true), true), options));
  EXPECT_FALSE(parse(catalog(entry("NEW", true) + ',' + entry("new", true), true), options));
  EXPECT_FALSE(parse(catalog(replace(entry("NEW", true), hash, "bad"), true), options));
  EXPECT_FALSE(parse(catalog(replace(entry("NEW", true), "French", ""), true), options));
  EXPECT_FALSE(parse(catalog(replace(entry("NEW", true), "\"bytes\":1", "\"bytes\":262145"), true), options));
  EXPECT_FALSE(parse(catalog(entry("NEW", true), true) + "{}", options));
  options.supportedTranslation = [](const char*, void*) { return false; };
  auto empty = std::make_unique<Catalog>();
  Parser emptyParser(*empty, options);
  ASSERT_TRUE(emptyParser.feed(input.data(), input.size()));
  ASSERT_TRUE(emptyParser.finish());
  EXPECT_EQ(empty->count, 0u);
  std::string entries;
  for (size_t i = 0; i <= MAX_ENTRIES; ++i) {
    if (!entries.empty()) entries += ',';
    entries += entry("NEW" + std::to_string(i), true);
  }
  EXPECT_FALSE(parse(catalog(entries, true), options));
}

TEST(AssetCatalog, SizeAndHashBounds) {
  for (const auto* number : {"0", "262145", "4294967295", "4294967296", "999999999999999999999"})
    EXPECT_FALSE(parse(replace(catalog(), "\"bytes\":1", std::string("\"bytes\":") + number))) << number;
  EXPECT_TRUE(parse(replace(catalog(), "\"bytes\":1", "\"bytes\":262144")));
  for (const auto& bad : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'A'), std::string(64, 'g')})
    EXPECT_FALSE(parse(replace(catalog(), hash, bad)));
  EXPECT_FALSE(parse(replace(catalog(), revision, std::string(40, 'C'))));
  for (const auto* bad : {"../notices.json", "https://evil/notices.json", "NOTICES.json"})
    EXPECT_FALSE(parse(replace(catalog(), "notices.json", bad)));
}
TEST(AssetCatalog, EntryCountCollisionAndTotalBodyCaps) {
  std::string entries;
  for (const auto* code : {"fr", "de", "ru", "es", "it", "pl", "pt", "sv", "uk"}) {
    if (!entries.empty()) entries += ',';
    entries += entry(code);
  }
  EXPECT_TRUE(parse(catalog(entries)));
  EXPECT_FALSE(parse(catalog(entries + ',' + entry("fr"))));
  EXPECT_FALSE(parse(catalog(entry() + ',' + entry())));
  EXPECT_FALSE(parse(catalog(entry("FR", true) + ',' + entry("fr", true), true), translationOptions()));
  entries.clear();
  for (size_t i = 0; i < MAX_ENTRIES; ++i) {
    if (!entries.empty()) entries += ',';
    entries += entry("L" + std::to_string(i), true);
  }
  EXPECT_TRUE(parse(catalog(entries, true), translationOptions()));
  EXPECT_FALSE(parse(catalog(entries + ',' + entry("L48", true), true), translationOptions()));
  auto exact = catalog();
  exact.resize(MAX_CATALOG_BYTES, ' ');
  EXPECT_TRUE(parse(exact));
  EXPECT_FALSE(parse(exact + ' '));
  EXPECT_FALSE(parse(exact + ' ', {}, exact.size() + 1));
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, {});
  EXPECT_TRUE(parser.feed(nullptr, 0));
  EXPECT_FALSE(parser.feed(nullptr, 1));
  EXPECT_LT(sizeof(Catalog) + sizeof(Parser), 16384u);
}
TEST(AssetCatalog, UnicodeEscapesAndDecodedIdentity) {
  EXPECT_TRUE(parse(replace(catalog(), "\"fr\"", "\"\\u0066r\"")));
  auto input = catalog(entry("FR", true), true);
  const auto options = translationOptions();
  for (const auto* name : {"Español", "\\u010c\\u0065ština", "\\ud83d\\ude00", "quote\\\"slash\\/back\\\\", "العربية"})
    EXPECT_TRUE(parse(replace(input, "French", name), options)) << name;
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, options);
  input = replace(input, "French", "\\u00e9\\ud83d\\ude00");
  ASSERT_TRUE(parser.feed(input.data(), input.size()));
  ASSERT_TRUE(parser.finish());
  EXPECT_STREQ(output->entries[0].name, "é😀");
  EXPECT_FALSE(parse(catalog(entry("FR", true) + ',' + entry("\\u0046R", true), true), options));
}
TEST(AssetCatalog, InvalidUnicodeControlsAndDecodedByteLimits) {
  const auto input = catalog(entry("FR", true), true);
  const auto options = translationOptions();
  for (const auto& name : {std::string("\\ud800"),
                           std::string("\\udc00"),
                           std::string("\\ud800\\u0041"),
                           std::string("\\ud800x"),
                           std::string("\\ud800\\x0000"),
                           std::string("\\uZZZZ"),
                           std::string("\\x20"),
                           std::string("\\u0000"),
                           std::string("\\u007f"),
                           std::string("\\u0080"),
                           std::string("\\b"),
                           std::string("\\n"),
                           std::string("\xc0\x80"),
                           std::string("\xe0\x80\x80"),
                           std::string("\xed\xa0\x80"),
                           std::string("\xf4\x90\x80\x80"),
                           std::string("\x80"),
                           std::string("\xf5\x80\x80\x80"),
                           std::string("\xc2"),
                           std::string("\xc2\\u00a0"),
                           std::string("\xe2"),
                           std::string("\xc2\x9f"),
                           std::string("\n"),
                           std::string(""),
                           std::string(96, 'a')})
    EXPECT_FALSE(parse(replace(input, "French", name), options)) << name;
  EXPECT_TRUE(parse(replace(input, "French", std::string(95, 'a')), options));
  std::string unicode;
  for (size_t i = 0; i < 47; ++i) unicode += "\\u00e9";
  EXPECT_TRUE(parse(replace(input, "French", unicode + "a"), options));
  EXPECT_FALSE(parse(replace(input, "French", unicode + "\\u00e9"), options));
  EXPECT_TRUE(parse(catalog(entry("A" + std::string(30, 'b'), true), true), options));
  EXPECT_FALSE(parse(catalog(entry("A" + std::string(31, 'b'), true), true), options));
}
TEST(AssetCatalog, PreviousHashClassificationAndImmutablePath) {
  const auto withPrevious = replace(entry(), "}", ",\"previous\":\"" + oldHash + "\"}");
  auto output = std::make_unique<Catalog>();
  Parser parser(*output, {});
  const auto input = catalog(withPrevious);
  ASSERT_TRUE(parser.feed(input.data(), input.size()));
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(classifySource(output->entries[0], hash.c_str()), SourceStatus::Current);
  EXPECT_EQ(classifySource(output->entries[0], oldHash.c_str()), SourceStatus::Previous);
  EXPECT_EQ(classifySource(output->entries[0], std::string(64, 'c').c_str()), SourceStatus::Different);
  EXPECT_EQ(classifySource(output->entries[0], nullptr), SourceStatus::Different);
  EXPECT_EQ(classifySource(output->entries[0], ""), SourceStatus::Different);
  char path[160];
  ASSERT_TRUE(immutableAssetPath(*output, output->entries[0], path, sizeof(path)));
  EXPECT_EQ(std::string(path), "snapshots/" + revision + "/hyphenation/" + hash + ".cphyph");
  const size_t length = std::strlen(path);
  EXPECT_FALSE(immutableAssetPath(*output, output->entries[0], path, length));
  EXPECT_STREQ(path, "");
  EXPECT_TRUE(immutableAssetPath(*output, output->entries[0], path, length + 1));
  EXPECT_FALSE(parse(catalog(replace(withPrevious, oldHash, hash))));
  EXPECT_FALSE(
      parse(catalog(replace(withPrevious, "\"previous\":\"" + oldHash + "\"", "\"previous\":[\"" + oldHash + "\"]"))));
  output->kind = Kind::Translation;
  ASSERT_TRUE(immutableAssetPath(*output, output->entries[0], path, sizeof(path)));
  EXPECT_EQ(std::string(path), "snapshots/" + revision + "/translation/" + hash + ".yaml");
}
}  // namespace
