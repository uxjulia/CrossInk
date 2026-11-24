#include "I18n.h"

#include <AssetCacheReadSession.h>
#include <HalStorage.h>
#include <I18nStrings.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(SIMULATOR)
#include <chrono>
#else
#include <esp_timer.h>
#endif

namespace {
using namespace language_cache;
constexpr size_t KEY_COUNT = static_cast<size_t>(StrId::_COUNT);
static_assert(KEY_COUNT <= MAX_KEYS);
constexpr size_t MAX_FILENAME = 128;

Key keyAt(size_t i) {
  const auto& entry = TRANSLATION_KEYS[i];
  return {entry.keyHash, entry.englishHash, entry.name, entry.id, entry.formatted};
}
const char* english(uint16_t id) { return i18n_strings::STRINGS_EN_DATA + i18n_strings::OFFSETS_EN[id]; }
const Schema SCHEMA{KEY_COUNT, keyAt, english};
int readInput(void* context, void* bytes, size_t count) { return static_cast<HalFile*>(context)->read(bytes, count); }
bool readFlash(void* context, size_t offset, void* bytes, size_t count) {
  return static_cast<AssetCacheStorage*>(context)->read(offset, bytes, count);
}
bool writeFlash(void* context, size_t offset, const void* bytes, size_t count) {
  return static_cast<AssetCacheStorage*>(context)->write(offset, bytes, count);
}
bool eraseFlash(void* context, size_t offset, size_t count) {
  return static_cast<AssetCacheStorage*>(context)->erase(offset, count);
}
bool verifyFlash(void* context, size_t offset, const void* bytes, size_t count) {
  return static_cast<AssetCacheStorage*>(context)->verify(offset, bytes, count);
}
uint64_t microseconds() {
#if defined(SIMULATOR)
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
#else
  return esp_timer_get_time();
#endif
}
}  // namespace

I18n::I18n() { selectBuiltin(Language::EN); }
void I18n::selectBuiltin(Language language) {
  if (!isLanguageBuiltIn(language)) language = Language::EN;
  const auto index = static_cast<uint8_t>(language);
  builtin_ = getLanguageStrings(language);
  std::strcpy(active_.code, LANGUAGE_CODES[index]);
  std::strcpy(active_.name, LANGUAGE_NAMES[index]);
  std::strcpy(active_.keyboard, LANGUAGE_CODES[index]);
  active_.rtl = language == Language::HE || language == Language::AR || language == Language::FA;
}
I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}
const char* I18n::get(StrId id) const {
  const size_t index = static_cast<size_t>(id);
  if (index >= KEY_COUNT) return "???";
  if (activeData_) {
    if (offsets_[index] != MISSING) return reinterpret_cast<const char*>(activeData_ + offsets_[index]);
    return english(static_cast<uint16_t>(index));
  }
  const uint16_t offset = builtin_.offsets[index];
  // Reserve one offset for English fallback; localized blobs may use the other 16 bits.
  return offset == I18N_ENGLISH_FALLBACK_OFFSET ? english(static_cast<uint16_t>(index)) : builtin_.data + offset;
}
bool I18n::hasTranslation(StrId id) const {
  const size_t index = static_cast<size_t>(id);
  if (index >= KEY_COUNT) return false;
  if (std::strcmp(active_.code, "EN") == 0) return true;
  if (activeData_) return offsets_[index] != MISSING;
  return builtin_.offsets[index] != I18N_ENGLISH_FALLBACK_OFFSET;
}
void I18n::begin(const char* preferredCode, uint64_t generation) {
  if (begun_) return;
  begun_ = true;
  if (!preferredCode || std::strcmp(preferredCode, "EN") == 0) return;
  selectBuiltin(languageFromCode(preferredCode));
  LOG_INF("LANG", "Built-in fallback %s", active_.code);
  const uint64_t start = microseconds();
  if (!flash_.begin()) {
    LOG_ERR("LANG", "Cannot open cached language %s; using %s", preferredCode, active_.name);
    return;
  }
  AssetCacheReadSession reads(flash_);
  if (!reads) {
    LOG_ERR("LANG", "Cannot read cached language %s; using %s", preferredCode, active_.name);
    return;
  }
  Installed selected;
  for (int slot = 0; slot < 2; ++slot) {
    AssetCacheStorage::Mapping candidate;
    if (!flash_.map(slotOffset(flash_.size(), slot), SLOT_SIZE, candidate)) {
      LOG_ERR("LANG", "Cannot map language slot %d", slot);
      continue;
    }
    Installed info;
    if (language_cache::open(candidate.data, candidate.size, SCHEMA, info) &&
        std::strcmp(info.metadata.code, preferredCode) == 0 && (!generation || info.generation == generation) &&
        info.generation > selected.generation) {
      AssetCacheStorage::unmap(mapping_);
      mapping_ = candidate;
      candidate = {};
      selected = info;
      slot_ = slot;
    }
    AssetCacheStorage::unmap(candidate);
  }
  if (!mapping_.data || !language_cache::open(mapping_.data, mapping_.size, SCHEMA, selected, offsets_)) {
    AssetCacheStorage::unmap(mapping_);
    slot_ = -1;
    LOG_ERR("LANG", "Cached language %s unavailable; using %s", preferredCode, active_.name);
    return;
  }
  activeData_ = mapping_.data;
#if defined(CROSSINK_LANGUAGE_BACKEND_PSRAM)
  // Explicit diagnostic selection, never an implicit C3 internal-heap fallback.
  psram_ = makePsramByteBufferNoThrow(SLOT_SIZE);
  if (psram_) {
    std::memcpy(psram_.get(), mapping_.data, SLOT_SIZE);
    activeData_ = psram_.get();
    LOG_INF("LANG", "Diagnostic PSRAM backend active (%u bytes)", unsigned(SLOT_SIZE));
  } else {
    LOG_ERR("LANG", "Diagnostic PSRAM allocation unavailable; retaining mapped flash");
  }
#endif
  active_ = selected.metadata;
  generation_ = selected.generation;
  LOG_INF("LANG", "Mapped %s in %llu us; index=%u bytes, generation=%llu", active_.code,
          static_cast<unsigned long long>(microseconds() - start), static_cast<unsigned>(sizeof(offsets_)),
          static_cast<unsigned long long>(generation_));
#if defined(CROSSINK_LANGUAGE_BENCHMARK)
  benchmark();
#endif
}
const char* I18n::getLanguageName(Language lang) const {
  const size_t index = static_cast<size_t>(lang);
  return index < getLanguageCount() ? LANGUAGE_NAMES[index] : "???";
}
Language I18n::languageFromCode(const char* code) {
  if (code)
    for (uint8_t i = 0; i < getLanguageCount(); ++i)
      if (std::strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  return Language::EN;
}
const char* I18n::getCharacterSet(Language language) {
  const auto index = static_cast<uint8_t>(language);
  return CHARACTER_SETS[index < getLanguageCount() ? index : 0];
}

static_assert(sizeof(I18n::Catalog) < 3072);
void I18n::Catalog::reset() {
  count = 0;
  used = 1;
  text[0] = '\0';
  cachedGeneration = 0;
}
uint16_t I18n::Catalog::append(const char* value) {
  if (!*value) return 0;
  const auto offset = static_cast<uint16_t>(used);
  const size_t n = std::strlen(value) + 1;
  std::memcpy(text + used, value, n);
  used += n;
  return offset;
}
bool I18n::Catalog::add(const char* code, const char* name, const char* filename, bool cached) {
  const size_t bytes = std::strlen(code) + std::strlen(name) + std::strlen(filename) + 3;
  if (count == CAPACITY || bytes > TEXT_BYTES - used) return false;
  entries[count++] = {append(code), append(name), append(filename), false, cached};
  return true;
}
bool I18n::Catalog::source(size_t index, const char* name, const char* filename) {
  const size_t bytes = std::strlen(name) + std::strlen(filename) + 2;
  if (bytes > TEXT_BYTES - used) return false;
  entries[index].name = append(name);
  entries[index].file = append(filename);
  return true;
}
void I18n::Catalog::select(size_t index, Option& result) const {
  if (index >= count) return;
  std::strcpy(result.code, code(index));
  std::strcpy(result.name, name(index));
  result.path[0] = '\0';
  if (*file(index)) std::snprintf(result.path, sizeof(result.path), "%s/%s", DIRECTORY, file(index));
  result.generation = entries[index].cached ? cachedGeneration : 0;
  result.duplicate = entries[index].duplicate;
}
Result I18n::discover(Catalog& catalog, void* context, bool (*continueWork)(void*),
                      const SourceProjection* projection) const {
  catalog.reset();
  bool cachedAdded = false;
  for (uint8_t index = 0; index < getLanguageCount(); ++index) {
    if (!isLanguageBuiltIn(static_cast<Language>(index))) continue;
    const bool cached = mapping_.data && std::strcmp(active_.code, LANGUAGE_CODES[index]) == 0;
    if (!catalog.add(LANGUAGE_CODES[index], cached ? active_.name : LANGUAGE_NAMES[index], "", cached))
      return Result::TooLarge;
    cachedAdded |= cached;
  }
  if (mapping_.data && !cachedAdded && !catalog.add(active_.code, active_.name, "", true)) return Result::TooLarge;
  catalog.cachedGeneration = generation_;
  const char* projectedFilename = nullptr;
  if (projection) {
    if (!projection->path || !projection->metadata) return Result::Invalid;
    const size_t prefix = std::strlen(DIRECTORY);
    if (std::strncmp(projection->path, DIRECTORY, prefix) || projection->path[prefix] != '/') return Result::Invalid;
    projectedFilename = projection->path + prefix + 1;
    const size_t length = std::strlen(projectedFilename);
    if (length < 6 || length >= MAX_FILENAME - 1 || std::strchr(projectedFilename, '/') ||
        std::strchr(projectedFilename, '\\') || std::strcmp(projectedFilename + length - 5, ".yaml"))
      return Result::Invalid;
  }
  auto addSource = [&](const Metadata& metadata, const char* filename) {
    size_t existing = 1;
    while (existing < catalog.count && std::strcmp(catalog.code(existing), metadata.code) != 0) ++existing;
    if (existing < catalog.count) {
      if (*catalog.file(existing)) {
        catalog.entries[existing].duplicate = true;
        LOG_ERR("LANG", "Duplicate language code %s", metadata.code);
        return true;
      }
      return catalog.source(existing, metadata.name, filename);
    }
    return catalog.add(metadata.code, metadata.name, filename);
  };
  if (continueWork && !continueWork(context)) return Result::Cancelled;
  auto directory = Storage.open(DIRECTORY);
  if (!directory || !directory.isDirectory()) {
    const bool allocationFailed = directory.allocationFailed();
    directory.close();
    if (allocationFailed) return Result::Memory;
    // Existing offline behavior permits a missing directory. A new projected
    // source still consumes a row and text even before the directory exists.
    if (projection && !addSource(*projection->metadata, projectedFilename)) return Result::TooLarge;
    return Result::Ok;
  }
  Inspector inspector;
  if (!inspector.available()) {
    LOG_ERR("LANG", "OOM: language metadata workspace");
    directory.close();
    return Result::Memory;
  }
  Result status = Result::Ok;
  size_t files = 0;
  bool projected = false;
  while (true) {
    if (continueWork && !continueWork(context)) {
      status = Result::Cancelled;
      break;
    }
    auto file = directory.openNextFile();
    if (!file) {
      if (directory.allocationFailed())
        status = Result::Memory;
      else if (directory.iterationFailed())
        status = Result::Io;
      file.close();
      break;
    }
    char filename[MAX_FILENAME];
    const size_t nameLength = file.getName(filename, sizeof(filename));
    if (file.isDirectory() || !nameLength || nameLength >= sizeof(filename) - 1 || isAppleDoubleFile(filename)) {
      file.close();
      continue;
    }
    const size_t length = std::strlen(filename);
    if (length < 6 || std::strcmp(filename + length - 5, ".yaml") != 0) {
      file.close();
      continue;
    }
    if (++files > Catalog::MAX_SOURCE_FILES) {
      status = Result::TooLarge;
      file.close();
      break;
    }
    Metadata metadata;
    Result result;
    if (projection && std::strcmp(filename, projectedFilename) == 0) {
      metadata = *projection->metadata;
      projected = true;
      result = Result::Ok;
    } else {
      struct ReadContext {
        HalFile* file;
        void* context;
        bool (*proceed)(void*);
        bool cancelled = false;
      } readContext{&file, context, continueWork};
      const auto read = [](void* opaque, void* bytes, size_t count) -> int {
        auto& current = *static_cast<ReadContext*>(opaque);
        if (current.proceed && !current.proceed(current.context)) {
          current.cancelled = true;
          return -1;
        }
        return current.file->read(bytes, count);
      };
      result = file.fileSize() > MAX_SOURCE_SIZE ? Result::TooLarge : inspector.inspect({&readContext, read}, metadata);
      if (readContext.cancelled) result = Result::Cancelled;
    }
    const bool closed = file.close();
    if (result == Result::Cancelled || (projection && (!closed || result == Result::Io || result == Result::Memory))) {
      status = result == Result::Cancelled ? result : !closed ? Result::Io : result;
      break;
    }
    if (result != Result::Ok) {
      LOG_ERR("LANG", "%s: %s", filename, resultName(result));
      continue;
    }
    if (!addSource(metadata, filename)) {
      status = Result::TooLarge;
      break;
    }
  }
  if (status == Result::Ok && projection && !projected) {
    if (++files > Catalog::MAX_SOURCE_FILES || !addSource(*projection->metadata, projectedFilename))
      status = Result::TooLarge;
  }
  if (!directory.close() && projection) status = Result::Io;
  if (continueWork && !continueWork(context)) status = Result::Cancelled;
  if (status != Result::Ok) {
    LOG_ERR("LANG", "Cannot show language catalog: %s (limit=%u text bytes, %u files)", resultName(status),
            unsigned(Catalog::TEXT_BYTES), unsigned(Catalog::MAX_SOURCE_FILES));
    return status;
  }
  std::sort(catalog.entries + 1, catalog.entries + catalog.count,
            [&catalog](const Catalog::Entry& a, const Catalog::Entry& b) {
              return std::strcmp(catalog.text + a.name, catalog.text + b.name) < 0;
            });
  return Result::Ok;
}
const char* I18n::sourceContract() { return i18n_strings::SOURCE_SHA256; }
const char* I18n::keysetContract() { return i18n_strings::KEYSET_SHA256; }
bool I18n::supportedDownload(const char* code) {
  if (!code) return false;
  for (size_t i = 1; i < getLanguageCount(); ++i)
    if (!std::strcmp(code, LANGUAGE_CODES[i])) return true;
  return false;
}
Result I18n::validateSource(const char* path, const char* code, const char* source, const char* keyset, void* context,
                            bool (*continueWork)(void*)) const {
  if (!supportedDownload(code) || !source || !keyset || std::strcmp(source, sourceContract()) ||
      std::strcmp(keyset, keysetContract())) {
    LOG_ERR("LANG", "Download source contract mismatch");
    return Result::Invalid;
  }
  HalFile file;
  if (!Storage.openFileForRead("LANG", path, file)) return Result::Io;
  Metadata metadata;
  ValidationOptions options;
  options.expectedCode = code;
  options.sourceSha256 = source;
  options.keysetSha256 = keyset;
  options.context = context;
  options.continueWork = continueWork;
  const auto result =
      file.fileSize() > MAX_SOURCE_SIZE ? Result::TooLarge : validate({&file, readInput}, SCHEMA, metadata, options);
  file.close();
  if (result != Result::Ok) LOG_ERR("LANG", "Download validation: %s", resultName(result));
  return result;
}
Result I18n::prepare(const char* path, Installed& result) {
  if (prepared_) {
    LOG_ERR("LANG", "Restart required before another language slot update");
    return Result::RestartRequired;
  }
  HalFile file;
  if (!Storage.openFileForRead("LANG", path, file)) return Result::Io;
  if (file.fileSize() > MAX_SOURCE_SIZE) {
    file.close();
    LOG_ERR("LANG", "Language source too large");
    return Result::TooLarge;
  }
  if (!flash_.begin(true)) {
    file.close();
    LOG_ERR("LANG", "Language flash unavailable");
    return Result::StorageUnavailable;
  }
  // One temporary 64 KiB PSRAM slot avoids per-string SD writes/flushes.
  // Too large for the stack; C3 and compatible-flash installs keep their old path.
  HeapByteBuffer staging;
  if (flash_.usesSd()) {
#if defined(SIMULATOR)
    staging = makeHeapByteBufferNoThrow(SLOT_SIZE);
#else
    staging = makePsramByteBufferNoThrow(SLOT_SIZE);
#endif
    if (!staging) {
      LOG_ERR("LANG", "Cannot allocate %u bytes of PSRAM for language install", unsigned(SLOT_SIZE));
      file.close();
      return Result::Memory;
    }
  }
  const uint64_t start = microseconds();
  const Flash flash{&flash_, flash_.size(), readFlash, writeFlash, eraseFlash, flash_.usesSd() ? verifyFlash : nullptr};
  const auto status = install({&file, readInput, [](void* context) { return static_cast<HalFile*>(context)->seek(0); }},
                              SCHEMA, flash, slot_, result, staging.get());
  if (status == Result::Ok) prepared_ = true;
  file.close();
  if (status == Result::Unchanged)
    LOG_INF("LANG", "Language already active; no flash update required");
  else if (status != Result::Ok)
    LOG_ERR("LANG", "Install failed: %s", resultName(status));
  else
    LOG_INF("LANG", "Prepared %s in %llu us, slot=%d generation=%llu", result.metadata.code,
            static_cast<unsigned long long>(microseconds() - start), result.slot,
            static_cast<unsigned long long>(result.generation));
  return status;
}
void I18n::benchmark() const {
  if (!mapping_.data) return;
  constexpr size_t SAMPLES = 31;
  constexpr size_t ROUNDS = 8;
  bool matchesEnglish = true;
  for (size_t i = 0; i < KEY_COUNT; ++i)
    if (std::strcmp(get(static_cast<StrId>(i)), english(static_cast<uint16_t>(i))) != 0) matchesEnglish = false;
  // One diagnostic-only 64 KiB PSRAM buffer, automatically released. Never
  // allocate this in internal RAM or in ordinary production lookup/rendering.
  auto psram = makePsramByteBufferNoThrow(SLOT_SIZE);
  if (psram) std::memcpy(psram.get(), mapping_.data, SLOT_SIZE);
  volatile uint32_t sink = 0;
  for (int mode = 0; mode < 3; ++mode) {
    if ((mode == 0 && !matchesEnglish) || (mode == 2 && !psram)) continue;
    uint32_t samples[SAMPLES];
    const uint8_t* data = mode == 2 ? psram.get() : mapping_.data;
    for (size_t sample = 0; sample < SAMPLES; ++sample) {
      const uint64_t start = microseconds();
      for (size_t round = 0; round < ROUNDS; ++round) {
        for (size_t i = 0; i < KEY_COUNT; ++i) {
          const char* text = mode == 0 || offsets_[i] == MISSING ? english(static_cast<uint16_t>(i))
                                                                 : reinterpret_cast<const char*>(data + offsets_[i]);
          // Same byte reads for each backend; volatile prevents hoisting repeated
          // walks out of the timing loop. This measures all strings, not one cache line.
          const volatile char* cursor = text;
          uint32_t sum = 0;
          while (*cursor) sum += static_cast<uint8_t>(*cursor++);
          sink = sink ^ sum;
        }
      }
      samples[sample] = static_cast<uint32_t>(microseconds() - start);
#ifndef SIMULATOR
      delay(1);  // allow the idle task to service the watchdog between samples
#endif
    }
    std::sort(std::begin(samples), std::end(samples));
    LOG_INF("LANG-BENCH", "backend=%s median_us=%u p95_us=%u rounds=%u strings=%u sink=%u",
            mode == 0   ? "embedded"
            : mode == 1 ? "mapped"
                        : "psram",
            unsigned(samples[SAMPLES / 2]), unsigned(samples[29]), unsigned(ROUNDS), unsigned(KEY_COUNT),
            unsigned(sink));
  }
  if (!matchesEnglish) LOG_INF("LANG-BENCH", "Install English benchmark to compare identical embedded/mapped strings");
}
