#pragma once

#include <AssetCacheStorage.h>
#include <I18nKeys.h>
#include <LanguageCache.h>

#include <cstdint>

#if defined(CROSSINK_LANGUAGE_BACKEND_PSRAM)
#include <Memory.h>
#endif

class I18n {
 public:
  static I18n& getInstance();
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  const char* get(StrId id) const;
  const char* operator[](StrId id) const { return get(id); }
  // True when the active language has its own text for id (English always does).
  bool hasTranslation(StrId id) const;
  // Called once before activities/rendering start. Mappings remain pinned until reboot.
  void begin(const char* preferredCode, uint64_t generation = 0);
  Language getLanguage() const { return languageFromCode(active_.keyboard); }
  const char* getCode() const { return active_.code; }
  const char* getName() const { return active_.name; }
  uint64_t getGeneration() const { return generation_; }
  bool isRightToLeft() const { return active_.rtl; }
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);
  static const char* getCharacterSet(Language lang);

  struct Option {
    char code[language_cache::CODE_SIZE] = {};
    char name[language_cache::NAME_SIZE] = {};
    char path[160] = {};
    uint64_t generation = 0;
    bool duplicate = false;
  };
  // One fallible, <3 KiB catalog. Strings share a bounded arena; the popup
  // borrows them. It is released before allocating installation scratch.
  struct Catalog {
    static constexpr size_t MAX_SOURCE_FILES = 64;
    static constexpr size_t CAPACITY = getLanguageCount() + MAX_SOURCE_FILES + 1;
    static constexpr size_t TEXT_BYTES = 2048;
    struct Entry {
      uint16_t code, name, file;
      bool duplicate, cached;
    };
    Entry entries[CAPACITY];
    char text[TEXT_BYTES];
    size_t count = 0, used = 1;
    uint64_t cachedGeneration = 0;
    void reset();
    bool add(const char* code, const char* name, const char* filename, bool cached = false);
    bool source(size_t index, const char* name, const char* filename);
    const char* code(size_t i) const { return text + entries[i].code; }
    const char* name(size_t i) const { return text + entries[i].name; }
    const char* file(size_t i) const { return text + entries[i].file; }
    bool disabled(size_t i) const { return entries[i].duplicate; }
    void select(size_t index, Option& result) const;

   private:
    uint16_t append(const char* value);
  };
  // Discovery/installation are explicit Settings actions, never boot/render work.
  struct SourceProjection {
    const char* path;
    const language_cache::Metadata* metadata;
  };
  // A projection previews publication without opening its payload or changing SD,
  // flash, or the active locale. Metadata and path are borrowed for this call.
  language_cache::Result discover(Catalog& catalog, void* context = nullptr, bool (*continueWork)(void*) = nullptr,
                                  const SourceProjection* projection = nullptr) const;
  // Unchanged returns the active identity without consuming the boot mutation guard.
  language_cache::Result prepare(const char* path, language_cache::Installed& result);
  // Download verification only: no flash access, settings writes or activation.
  language_cache::Result validateSource(const char* path, const char* code, const char* source, const char* keyset,
                                        void* context = nullptr, bool (*continueWork)(void*) = nullptr) const;
  static const char* sourceContract();
  static const char* keysetContract();
  static bool supportedDownload(const char* code);
  // Diagnostic only: compares identical bytes in embedded flash, mapped flash,
  // and (where available) PSRAM. No backend switch or timing on the hot path.
  void benchmark() const;

 private:
  I18n();
  void selectBuiltin(Language language);
  LangStrings builtin_;
  AssetCacheStorage flash_;
  AssetCacheStorage::Mapping mapping_;
  language_cache::Metadata active_;
  const uint8_t* activeData_ = nullptr;
#if defined(CROSSINK_LANGUAGE_BACKEND_PSRAM)
  HeapByteBuffer psram_;  // Diagnostic backend only; normal firmware keeps mapped flash.
#endif
  uint64_t generation_ = 0;
  int slot_ = -1;
  bool begun_ = false;
  bool prepared_ = false;  // Successful slot mutation pins this boot until restart.
  // 2 bytes/key in internal RAM, no string copies or per-lookup allocation.
  uint16_t offsets_[static_cast<size_t>(StrId::_COUNT)] = {};
};

#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
