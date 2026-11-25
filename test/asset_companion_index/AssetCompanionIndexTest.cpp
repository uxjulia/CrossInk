#include <AssetCompanionIndex.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace AssetCompanionIndex;
namespace {
constexpr char revision[] = "b154ea5dd7e28c6c8ce7765bdde89e2e3dbc9304";
const std::string prefix = std::string("snapshots/") + revision + "/source/";
const std::string digest(64, 'a');
std::string referenceJson(const std::string& suffix = "a.txt", uint32_t bytes = 1) {
  return "{\"path\":\"" + prefix + suffix + "\",\"sha256\":\"" + digest + "\",\"bytes\":" + std::to_string(bytes) + "}";
}
std::string doc(const std::string& refs, const std::string& metadata = "") {
  return "{\"hyphenation\":{\"de\":{\"files\":[" + refs + "]" + metadata + "}},\"translation\":{}}";
}
bool collect(const Reference& reference, void* context) {
  static_cast<std::vector<Reference>*>(context)->push_back(reference);
  return true;
}
bool parse(const std::string& json, size_t chunk = 1, std::vector<Reference>* output = nullptr,
           AssetCatalog::Kind kind = AssetCatalog::Kind::Hyphenation) {
  std::vector<Reference> local;
  Parser parser({kind, "de", revision, collect, output ? output : &local});
  for (size_t i = 0; i < json.size(); i += chunk) {
    if (!parser.feed(json.data() + i, std::min(chunk, json.size() - i))) return false;
  }
  return parser.finish();
}
void replace(std::string& text, const std::string& before, const std::string& after) {
  const auto at = text.find(before);
  ASSERT_NE(at, std::string::npos);
  text.replace(at, before.size(), after);
}
}  // namespace

TEST(AssetCompanionIndex, BoundedMemoryAndEveryChunkBoundary) {
  EXPECT_LE(sizeof(Parser), 1024u);
  const auto json =
      doc(referenceJson("review-bundle/a.zip", 1767291), ",\"licenseDeclaration\":\"Ä😀\\uD83D\\uDE00\\nnotice\"");
  for (size_t chunk = 1; chunk <= json.size(); ++chunk) {
    std::vector<Reference> out;
    ASSERT_TRUE(parse(json, chunk, &out)) << chunk;
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].path, (prefix + "review-bundle/a.zip").c_str());
    EXPECT_STREQ(out[0].sha256, digest.c_str());
    EXPECT_EQ(out[0].bytes, 1767291u);
  }
}
TEST(AssetCompanionIndex, ExactKindCodeAndRequiredFields) {
  EXPECT_TRUE(parse("{\"translation\":{\"de\":{\"files\":[" + referenceJson() + "]}}}", 1, nullptr,
                    AssetCatalog::Kind::Translation));
  for (const auto& json : {"{}", "[]", "{\"hyphenation\":{}}", "{\"hyphenation\":{\"de\":{}}}",
                           "{\"hyphenation\":{\"DE\":{\"files\":[]}}}", "{\"hyphenation\":{\"de\":{\"files\":[]}}}",
                           "{\"hyphenation\":{\"de\":false}}", "{\"hyphenation\":[]}"})
    EXPECT_FALSE(parse(json)) << json;
  EXPECT_FALSE(parse(doc(referenceJson()), 1, nullptr, AssetCatalog::Kind::Translation));
  for (const std::string field : {"path", "bytes", "sha256"}) {
    auto json = doc(referenceJson());
    replace(json, "\"" + field + "\":", "\"other\":");
    EXPECT_FALSE(parse(json)) << field;
  }
}
TEST(AssetCompanionIndex, DuplicateRequiredFieldsFailClosed) {
  for (const auto& field : {"path", "bytes", "sha256"}) {
    auto json = doc(referenceJson());
    const auto first = json.find("\"" + std::string(field) + "\":");
    const auto end = json.find_first_of(",}", first);
    json.insert(end, "," + json.substr(first, end - first));
    EXPECT_FALSE(parse(json));
  }
  EXPECT_FALSE(parse(doc(referenceJson(), ",\"files\":[" + referenceJson() + "]")));
  EXPECT_FALSE(parse("{\"hyphenation\":{\"de\":{\"files\":[" + referenceJson() + "]},\"de\":{\"files\":[" +
                     referenceJson() + "]}}}"));
  EXPECT_FALSE(parse(doc(referenceJson()).substr(0, doc(referenceJson()).size() - 1) + ",\"hyphenation\":{}}"));
  EXPECT_FALSE(parse(doc(referenceJson()).substr(0, doc(referenceJson()).size() - 1) + ",\"translation\":{}}"));
}
TEST(AssetCompanionIndex, ReferenceTypeAndNumericBounds) {
  EXPECT_TRUE(parse(doc(referenceJson("a", MAX_FILE_BYTES))));
  EXPECT_FALSE(parse(doc(referenceJson("a", MAX_FILE_BYTES + 1))));
  EXPECT_FALSE(parse(doc(referenceJson("a", 0))));
  for (const std::string value :
       {"-1", "1.0", "1e0", "01", "true", "null", "[]", "{}", "\"1\"", "999999999999999999999"}) {
    auto json = doc(referenceJson());
    replace(json, "\"bytes\":1", "\"bytes\":" + value);
    EXPECT_FALSE(parse(json)) << value;
  }
  for (const std::string value : {"null", "1", "[]", "{}", "true"}) {
    auto json = doc(referenceJson());
    replace(json, "\"" + digest + "\"", value);
    EXPECT_FALSE(parse(json));
  }
  auto json = doc(referenceJson());
  replace(json, digest, std::string(64, 'A'));
  EXPECT_FALSE(parse(json));
}
TEST(AssetCompanionIndex, ReferenceCountAndByteBudgetBoundaries) {
  std::string list;
  for (size_t i = 0; i < MAX_REFERENCES_PER_INDEX; ++i) {
    if (i) list += ',';
    list += referenceJson("file" + std::to_string(i));
  }
  EXPECT_TRUE(parse(doc(list)));
  EXPECT_FALSE(parse(doc(list + ',' + referenceJson())));
  list.clear();
  for (int i = 0; i < 4; ++i) {
    if (i) list += ',';
    list += referenceJson("a" + std::to_string(i), MAX_FILE_BYTES);
  }
  EXPECT_TRUE(parse(doc(list)));
  EXPECT_FALSE(parse(doc(list + ',' + referenceJson())));
}
TEST(AssetCompanionIndex, UnsafePathsAndExactRevision) {
  for (const std::string suffix :
       {"../a", "a/../b", "/a",  "a//b", "a/",  ".",         "..",    "a.",        "a ",   "a:b", "a\\\\b", "%2e%2e/a",
        "a?b",  "a*b",    "a|b", "a<b",  "a>b", "a\\u0000b", "é.txt", "a/CON.txt", "lPt9", "NUL", "a/aux/b"})
    EXPECT_FALSE(parse(doc(referenceJson(suffix)))) << suffix;
  auto json = doc(referenceJson());
  replace(json, revision, std::string(40, 'a'));
  EXPECT_FALSE(parse(json));
  json = doc(referenceJson());
  replace(json, "/source/", "/payload/");
  EXPECT_FALSE(parse(json));
  EXPECT_TRUE(parse(doc(referenceJson(std::string(MAX_PATH - prefix.size(), 'a')))));
  EXPECT_FALSE(parse(doc(referenceJson(std::string(MAX_PATH - prefix.size() + 1, 'a')))));
}
TEST(AssetCompanionIndex, LargeMetadataIsStreamedAndBounded) {
  EXPECT_TRUE(parse(doc(referenceJson(), ",\"licenseDeclaration\":\"" + std::string(65536, 'a') + "\""), 67));
  EXPECT_FALSE(parse(doc(referenceJson(), ",\"licenseDeclaration\":\"" + std::string(65537, 'a') + "\""), 67));
  EXPECT_TRUE(
      parse(doc(referenceJson(), ",\"arbitrary\":{\"list\":[true,false,null,-12.5e+4,{\"str\":\"\\u0000\"}]}")));
  EXPECT_FALSE(parse(doc(referenceJson(), ",\"" + std::string(65, 'k') + "\":0")));
  EXPECT_TRUE(parse(doc(referenceJson(), ",\"" + std::string(64, 'k') + "\":0")));
  EXPECT_FALSE(parse(doc(referenceJson(), ",\"a\":\"ok\",\"bad\\u0000key\":0")));
  std::string nested(17, '[');
  nested += "0";
  nested += std::string(17, ']');
  EXPECT_FALSE(parse(doc(referenceJson(), ",\"deep\":" + nested)));
}
TEST(AssetCompanionIndex, MalformedIgnoredMetadataFails) {
  for (const std::string value : {"01", "-", "1.", "1e", "1e+", "truex", "NaN", "undefined", "[1,]", "{\"a\":1,}", "[}",
                                  "{]", "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\"", "\"\\uXXXX\"", "\"\\x00\""})
    EXPECT_FALSE(parse(doc(referenceJson(), ",\"ignored\":" + value))) << value;
  for (const auto& value :
       {std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe2\x82", 2), std::string("\x80", 1), std::string("\0", 1)})
    EXPECT_FALSE(parse(doc(referenceJson(), ",\"ignored\":\"" + value + "\"")));
}
TEST(AssetCompanionIndex, WholeIndexLimitAndIncompletePrefixes) {
  const auto json = doc(referenceJson());
  for (size_t size = 0; size < json.size(); ++size) EXPECT_FALSE(parse(json.substr(0, size))) << size;
  EXPECT_FALSE(parse(json + "{}"));
  EXPECT_TRUE(parse(json + std::string(MAX_INDEX_BYTES - json.size(), ' '), 4096));
  EXPECT_FALSE(parse(json + std::string(MAX_INDEX_BYTES - json.size() + 1, ' '), 4096));
  EXPECT_FALSE(parse(doc(
                         referenceJson(), ",\"many\":[" + std::string(1, '0') +
                                              [] {
                                                std::string s;
                                                for (int i = 0; i < 70000; ++i) s += ",0";
                                                return s;
                                              }() +
                                              "]"),
                     4096));
}
TEST(AssetCompanionIndex, CallbackFailureAndFinishProtocol) {
  auto reject = [](const Reference&, void*) { return false; };
  Parser rejected({AssetCatalog::Kind::Hyphenation, "de", revision, reject, nullptr});
  const auto json = doc(referenceJson());
  EXPECT_FALSE(rejected.feed(json.data(), json.size()));
  EXPECT_NE(rejected.error(), nullptr);
  EXPECT_FALSE(rejected.finish());
  std::vector<Reference> out;
  Parser accepted({AssetCatalog::Kind::Hyphenation, "de", revision, collect, &out});
  EXPECT_TRUE(accepted.feed(json.data(), json.size()));
  EXPECT_EQ(accepted.receivedBytes(), json.size());
  EXPECT_TRUE(accepted.finish());
  EXPECT_FALSE(accepted.feed("", 0));
  Parser bad({AssetCatalog::Kind::Hyphenation, "DE TOO LONG OR UNSAFE", revision, collect, &out});
  EXPECT_FALSE(bad.feed(json.data(), json.size()));
}
TEST(AssetCompanionIndex, V3ThirteenReferencesAndOpaqueZip) {
  std::string refs;
  for (int i = 0; i < 13; ++i) {
    if (i) refs += ',';
    refs += referenceJson(i == 0 ? "review-bundle-d0371de11096fd6d4e6a14a4aaed4512ce1ab59be6848364f94438f31a4c0ed2.zip"
                                 : "tooling/f" + std::to_string(i),
                          i == 0 ? 1767291 : 100);
  }
  std::vector<Reference> output;
  EXPECT_TRUE(parse(doc(refs, ",\"review\":{\"status\":\"fixture\",\"maintained\":false}"), 19, &output));
  ASSERT_EQ(output.size(), 13u);
  EXPECT_EQ(output[0].bytes, 1767291u);
}
