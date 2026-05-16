#pragma once
#include <Print.h>
#include <expat.h>

#include <cstddef>
#include <string>

constexpr size_t MAX_OPDS_FEED_ENTRIES = 50;

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Lightweight read-only view over parsed OPDS entries.
 *
 * This keeps old range-for call sites working without bringing back a
 * heap-growing vector inside the parser.
 */
class OpdsEntryView {
 public:
  OpdsEntryView() = default;
  OpdsEntryView(const OpdsEntry* entries, size_t count) : entries(entries), count(count) {}

  const OpdsEntry* begin() const { return entries; }
  const OpdsEntry* end() const { return entries ? entries + count : nullptr; }
  const OpdsEntry* data() const { return entries; }
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  const OpdsEntry& operator[](size_t index) const { return entries[index]; }

 private:
  const OpdsEntry* entries = nullptr;
  size_t count = 0;
};

/**
 * Lightweight read-only view over parsed OPDS book entries.
 */
class OpdsBookView {
 public:
  class Iterator {
   public:
    Iterator(const OpdsEntry* current, const OpdsEntry* end) : current(current), end(end) { skipNonBooks(); }

    const OpdsEntry& operator*() const { return *current; }
    const OpdsEntry* operator->() const { return current; }
    Iterator& operator++() {
      if (current != end) ++current;
      skipNonBooks();
      return *this;
    }
    bool operator==(const Iterator& other) const { return current == other.current; }
    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    void skipNonBooks() {
      while (current != end && current->type != OpdsEntryType::BOOK) {
        ++current;
      }
    }

    const OpdsEntry* current = nullptr;
    const OpdsEntry* end = nullptr;
  };

  OpdsBookView() = default;
  OpdsBookView(const OpdsEntry* entries, size_t count) : entries(entries), count(count) {}

  Iterator begin() const { return Iterator(entries, entries ? entries + count : nullptr); }
  Iterator end() const {
    const OpdsEntry* endEntry = entries ? entries + count : nullptr;
    return Iterator(endEntry, endEntry);
  }
  bool empty() const { return size() == 0; }
  size_t size() const {
    size_t books = 0;
    for (const auto& entry : *this) {
      (void)entry;
      ++books;
    }
    return books;
  }

 private:
  const OpdsEntry* entries = nullptr;
  size_t count = 0;
};

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
 *   OpdsParser parser(entries);
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser(OpdsEntry* entries, size_t entryCapacity);
  template <size_t N>
  explicit OpdsParser(OpdsEntry (&entries)[N]) : OpdsParser(entries, N) {}
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;
  bool parse(const uint8_t* xmlData, size_t length);
  bool parse(const char* xmlData, size_t length) { return parse(reinterpret_cast<const uint8_t*>(xmlData), length); }

  bool error() const;

  operator bool() const { return !error(); }

  size_t getEntryCount() const { return entryCount; }
  const OpdsEntry* getEntry(size_t index) const {
    if (!entries || index >= entryCount) return nullptr;
    return &entries[index];
  }
  OpdsEntryView getEntries() const { return OpdsEntryView(entries, entryCount); }
  OpdsBookView getBooks() const { return OpdsBookView(entries, entryCount); }
  bool wasTruncated() const { return truncated; }

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  bool resetXmlParser();

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);

  XML_Parser parser = nullptr;
  OpdsEntry* entries = nullptr;
  size_t entryCapacity = 0;
  size_t entryCount = 0;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;

  bool errorOccured = false;
  bool truncated = false;
};
