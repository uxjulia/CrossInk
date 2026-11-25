#include <HalStorage.h>
#include <I18n.h>
#include <I18nStrings.h>
#include <Memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  if (std::strcmp(argv[1], "format") == 0) {
    if (argc != 4) return 2;
    std::printf("%d", language_cache::compatibleFormat(argv[2], argv[3]));
    return 0;
  }
  const char* code = argc > 2 ? argv[2] : "EN";
  const uint64_t generation = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
  I18N.begin(code, generation);
  if (std::strcmp(argv[1], "boot") == 0) {
    // SD-backed cache reads happen only at boot. Lookups must never do I/O.
    // Startup may probe and open the SD image once each, regardless of chunk
    // count. Both language mappings must release the shared reader on return.
    if (HalFile::liveFiles || HalStorage::opens > 2) return 3;
    const auto bootOpens = HalStorage::opens;
    const auto bootReads = HalFile::reads;
    for (int i = 0; i < 10000; ++i) (void)I18N.get(StrId::STR_SETTINGS_TITLE);
    if (HalStorage::opens != bootOpens || HalFile::reads != bootReads) return 4;
    std::printf("%s|%s|%s|%u\n", I18N.getCode(), I18N.getName(), tr(STR_SETTINGS_TITLE), I18N.isRightToLeft());
    I18N.benchmark();
  } else if (std::strcmp(argv[1], "lookup") == 0) {
    if (argc < 5) return 2;
    for (size_t i = 0; i < static_cast<size_t>(StrId::_COUNT); ++i) {
      if (std::strcmp(TRANSLATION_KEYS[i].name, argv[4]) == 0) {
        std::printf("%s", I18N.get(static_cast<StrId>(TRANSLATION_KEYS[i].id)));
        return 0;
      }
    }
    return 2;
  } else if (std::strcmp(argv[1], "validate") == 0) {
    if (argc < 5) return 2;
    const char* before = tr(STR_SETTINGS_TITLE);
    const auto result = I18N.validateSource(argv[4], code, I18n::sourceContract(), I18n::keysetContract());
    if (result != language_cache::Result::Ok) {
      std::fprintf(stderr, "%s\n", language_cache::resultName(result));
      return 5;
    }
    if (tr(STR_SETTINGS_TITLE) != before) return 6;
  } else if (std::strcmp(argv[1], "unchanged") == 0) {
    if (argc < 5) return 2;
    const char* old = tr(STR_SETTINGS_TITLE);
    language_cache::Installed installed;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (I18N.prepare(argv[4], installed) != language_cache::Result::Unchanged) return 5;
      if (installed.generation != generation || std::strcmp(installed.metadata.code, code) != 0 ||
          I18N.getGeneration() != generation || tr(STR_SETTINGS_TITLE) != old)
        return 6;
    }
    // Neither no-op may consume the one successful mutation allowed per boot.
    if (argc > 5) {
      if (I18N.prepare(argv[5], installed) != language_cache::Result::Ok) return 7;
      if (installed.generation != generation + 1 || tr(STR_SETTINGS_TITLE) != old) return 8;
      if (I18N.prepare(argv[5], installed) != language_cache::Result::RestartRequired) return 9;
    }
  } else if (std::strcmp(argv[1], "install") == 0) {
    if (argc < 5) return 2;
    const char* old = tr(STR_SETTINGS_TITLE);
    const std::string before(old);
    language_cache::Installed installed;
    const auto beforeSyncs = HalFile::syncs;
    const auto result = I18N.prepare(argv[4], installed);
    // The SD installation may initialize the cache once, then sync erase,
    // compiled data, and commit marker. Never flush per translated string.
    if (std::getenv("CROSSINK_TEST_BATCHED_INSTALL") && HalFile::syncs - beforeSyncs > 4) return 10;
    if (HalFile::liveFiles) return 11;
    if (result != language_cache::Result::Ok) {
      std::fprintf(stderr, "%s\n", language_cache::resultName(result));
      return 5;
    }
    if (std::strcmp(old, before.c_str()) != 0 || tr(STR_SETTINGS_TITLE) != old) return 6;
    language_cache::Installed another;
    if (I18N.prepare(argv[4], another) != language_cache::Result::RestartRequired) return 7;
    std::printf("%s|%llu\n", installed.metadata.code, static_cast<unsigned long long>(installed.generation));
  } else if (std::strcmp(argv[1], "scan") == 0) {
    HeapObject<I18n::Catalog> catalog;
    catalog.init(MemoryPool::None);
    const auto status = catalog ? I18N.discover(*catalog.get()) : language_cache::Result::Memory;
    if (status != language_cache::Result::Ok) {
      std::fprintf(stderr, "%s\n", language_cache::resultName(status));
      return 5;
    }
    for (size_t i = 0; i < catalog->count; ++i) {
      I18n::Option item;
      catalog->select(i, item);
      std::printf("%s|%s|%d|%s\n", item.code, item.name, item.duplicate, item.path);
    }
  } else
    return 2;
}
