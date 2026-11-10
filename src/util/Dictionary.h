#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Helper for constructing dictionary file paths from a folder base path.
struct DictPaths {
  const std::string& folder;
  explicit DictPaths(const std::string& f) : folder(f) {}

  std::string idx() const { return folder + ".idx"; }
  std::string dict() const { return folder + ".dict"; }
  std::string syn() const { return folder + ".syn"; }
  std::string ifo() const { return folder + ".ifo"; }
  std::string idxOft() const { return folder + ".idx.oft"; }
  std::string synOft() const { return folder + ".syn.oft"; }
  std::string idxOftCspt() const { return folder + ".idx.oft.cspt"; }
  std::string synOftCspt() const { return folder + ".syn.oft.cspt"; }
  std::string quickIdx() const { return folder + ".qidx"; }
  std::string dictDz() const { return folder + ".dict.dz"; }
  std::string synDz() const { return folder + ".syn.dz"; }
};

// Plain-function-pointer callbacks for Dictionary::lookup.
// Zero overhead: no heap allocation, no vtable, no std::function bloat.
struct DictLookupCallbacks {
  void* ctx = nullptr;
  void (*onProgress)(void* ctx, int percent) = nullptr;
  bool (*shouldCancel)(void* ctx) = nullptr;
};

// Metadata parsed from a StarDict .ifo file.
struct DictInfo {
  char bookname[128] = "";
  char website[128] = "";
  char date[32] = "";
  char description[256] = "";
  char sametypesequence[16] = "";
  uint32_t wordcount = 0;
  uint32_t altFormCount = 0;
  uint32_t idxfilesize = 0;
  uint32_t idxoffsetbits = 32;
  bool hasAltForms = false;
  bool isCompressed = false;  // .dict.dz present but no .dict
  char lang[32] = "";         // e.g. "en-en", "el-el"
  bool valid = false;
};

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  std::string folderPath;  // dictionary base path (e.g. /dictionary/dict-en-en/dict-data)
  std::string headword;    // matched .idx spelling, including the dictionary's capitalization
  uint32_t offset = 0;     // byte offset in .dict file
  uint32_t size = 0;       // byte length in .dict file
  bool found = false;
  // The index could not be opened, sought, or read to a complete entry. This
  // is not a genuine dictionary miss and must not be presented as one.
  bool readError = false;
};

struct DictDefinitionSlice {
  std::string folderPath;
  uint32_t offset = 0;
  uint32_t size = 0;
  char type = 'm';
  bool isHtml = false;
  bool found = false;
};

class Dictionary {
 public:
  static constexpr unsigned long LONG_PRESS_MS = 600;

  // Returns the active dictionary folder base path. A temporary lookup override
  // takes priority; otherwise resolves the per-book/global dictionary.bin files.
  // Returns empty string if no dictionary is configured or the file cannot be read.
  static std::string readDictPath(const char* cachePath = nullptr);

  // Returns the persisted per-book/global dictionary path without applying a
  // temporary lookup override.
  static std::string readConfiguredDictPath(const char* cachePath = nullptr);

  // Temporarily overrides dictionary resolution for the active lookup flow.
  // This is memory-only; callers must clear it when the lookup activity exits.
  static void setLookupDictPathOverride(const char* folderPath);
  static void clearLookupDictPathOverride();

  // Writes folderPath to /.crosspoint/dictionary.bin (global setting).
  // Pass empty string to clear the global dictionary.
  static void saveGlobalDictPath(const char* folderPath);

  // Returns true when /.crosspoint/dictionary.bin exists, even if it is empty
  // because the user explicitly selected "None".
  static bool hasGlobalDictPathFile();

  // Returns true if a dictionary is configured and all required files exist.
  static bool exists(const char* cachePath = nullptr);

  // Returns true if a .syn file exists for the active dictionary.
  // Gates all alternate-form UI — checked at runtime against the physical file.
  static bool hasAltForms(const char* cachePath = nullptr);

  // Validates the dictionary path stored in /.crosspoint/dictionary.bin against the SD card.
  // If the path is missing or the required files are gone, clears the file. Returns true if valid.
  static bool isValidDictionary();

  // Parse the .ifo file in folderPath and return metadata.
  // Also checks for .syn and .dict.dz presence.
  static DictInfo readInfo(const char* folderPath);

  // Build a small sampled .qidx sidecar when the active dictionary has no
  // desktop-generated .cspt/.oft accelerator. Failure is recoverable: callers
  // can continue to locate(), which falls back to scanning the full .idx.
  static bool prepareQuickIndex(const DictLookupCallbacks& cbs = {}, const char* cachePath = nullptr);

  // Search .idx via .cspt, .oft, or .qidx, trying the supplied spelling first,
  // then a Hebrew/Arabic vowel-stripped query on a miss. Index ordering is unchanged.
  static DictLocation locate(const std::string& word, const DictLookupCallbacks& cbs = {},
                             const char* cachePath = nullptr);

  // Search the exact word and then its stem variants while reusing one open
  // index/accelerator session. matchedStem is set only when a variant matched.
  static DictLocation locateWithStemVariants(const std::string& word, bool* matchedStem,
                                             const DictLookupCallbacks& cbs = {}, const char* cachePath = nullptr);

  // Look up word in .idx (via .idx.oft if present). Returns definition or empty string.
  static std::string lookup(const std::string& word, const DictLookupCallbacks& cbs = {},
                            const char* cachePath = nullptr);

  // Resolve the displayable text field inside a StarDict .dict entry. Some
  // dictionaries store typed multi-field entries, so the raw .idx byte range can
  // include type bytes, null terminators, or binary resources that must not be
  // rendered as text.
  static DictDefinitionSlice resolveDefinitionSlice(const DictLocation& location, const DictInfo& info);

  // Look up word in .syn (via .syn.oft if present).
  // Returns the canonical headword from .idx, or empty string if not found.
  static std::string resolveAltForm(const std::string& word, const char* cachePath = nullptr);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; uses .idx.oft if present for neighbourhood search.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults, const char* cachePath = nullptr);

  // Reads .idx.oft.cspt header and returns entryCount.
  // Returns 0 if the file is missing, too small, or has invalid magic/version.
  // Cheap: one SD seek, 12 bytes read.
  static uint32_t readCsptEntryCount(const char* cachePath = nullptr);

 private:
  // Shared word read buffer. Lookup functions are single-threaded; this avoids
  // putting a 256-byte array on the stack in every caller (and 512B peak when nested).
  static char wordBuf[256];

  enum class LookupAccelerator : uint8_t { None, Cspt, Oft, QuickIndex };

  struct LookupSession {
    std::string folderPath;
    HalFile idx;
    HalFile accelerator;
    uint32_t idxFileSize = 0;
    uint32_t qidxSampleCount = 0;
    uint8_t suffixBytes = 8;
    LookupAccelerator acceleratorKind = LookupAccelerator::None;
  };

  static std::string resolveExactAltForm(const std::string& word, const char* cachePath);
  static DictLocation locateExactInSession(LookupSession& session, const std::string& word,
                                           const DictLookupCallbacks& cbs);

  static bool openLookupSession(LookupSession& session, const char* cachePath);
  static void closeLookupSession(LookupSession& session);
  static DictLocation locateInSession(LookupSession& session, const std::string& word, const DictLookupCallbacks& cbs);

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(HalFile& file, char* buf, size_t bufSize);

  // Read the word at ordinal `ordinal` in .idx.
  // folderPath is the dictionary base path (e.g. /dictionary/dict-en-en/dict-data).
  static std::string wordAtOrdinal(const std::string& folderPath, uint32_t ordinal);

  static std::string readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size);

  // Binary search .oft to find the page boundary bytes in src containing target.
  // On return, *startByte and *endByte delimit the 32-word page to scan linearly.
  // srcFileSize is used as the upper bound when the page is the last one.
  static void findPageBounds(HalFile& oft, HalFile& src, uint32_t srcFileSize, const char* target, uint32_t* startByte,
                             uint32_t* endByte, bool startBeforeCaseMatches = false);

  // Binary search .idx.oft.cspt to find the scan range in .idx containing target.
  // Returns true if .cspt was valid and bounds were set, false to fall back to .oft.
  static bool binarySearchCspt(HalFile& cspt, const char* target, uint32_t idxFileSize, uint32_t* startByte,
                               uint32_t* endByte, bool startBeforeCaseMatches);

  // Binary search a device-generated sampled .qidx sidecar.
  static bool binarySearchQuickIndex(HalFile& qidx, HalFile& idx, uint32_t sampleCount, const char* target,
                                     uint32_t idxFileSize, uint32_t* startByte, uint32_t* endByte,
                                     uint32_t* matchedSample = nullptr);

  static bool buildQuickIndex(HalFile& idx, uint32_t idxFileSize, uint8_t suffixBytes, const char* qidxPath,
                              const DictLookupCallbacks& cbs);

  // Resolve the byte range in src to scan for target. Tries .cspt first; on miss or
  // absence, falls back to .oft. Callers must initialize *startByte=0, *endByte=srcFileSize
  // before calling — when both are absent, the bounds are left untouched. Passing
  // startBeforeCaseMatches backs up one sample so exact-case lookup can inspect every
  // equivalent spelling. Used by both .idx and .syn; .qidx is handled by locate afterward.
  static bool resolveScanBounds(const char* csptPath, const char* oftPath, HalFile& src, uint32_t srcFileSize,
                                const char* target, uint32_t* startByte, uint32_t* endByte,
                                bool startBeforeCaseMatches);

  static int editDistance(const std::string& a, const std::string& b, int maxDist, int* dp);
};
