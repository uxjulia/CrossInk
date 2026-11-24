#ifdef SIMULATOR
#include "StatsUploadSmokeTest.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SilentRestart.h"
#include "activities/network/StatsUploadActivity.h"
#include "activities/reader/DailyReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/ReadingSessionJournal.h"
#include "activities/reader/StatsBackup.h"
#include "activities/reader/StatsUploadPayload.h"
#include "activities/settings/BackupStatsActivity.h"
#include "network/ReadingSyncUpload.h"
#include "network/StatsUploadClient.h"
#include "network/UsbProgressSync.h"
#include "util/BookMoveUtils.h"
#include "util/StatsStorage.h"

namespace {
void require(bool ok, const char* what) {
  if (!ok) {
    LOG_ERR("SMOKE", "Stats upload contract failed: %s", what);
    std::_Exit(2);
  }
}
}  // namespace

void verifyStatsUploadContract() {
  if (const char* mode = std::getenv("CROSSINK_STATS_TEST_RESET")) {
    const char* server = std::getenv("CROSSINK_STATS_TEST_SERVER");
    const char* user = std::getenv("CROSSINK_STATS_TEST_USER");
    const char* password = std::getenv("CROSSINK_STATS_TEST_PASSWORD");
    require(server && user && password, "explicit reset test credentials");
    KOREADER_STORE.setCredentials(user, password);
    KOREADER_STORE.setServerUrl(server);
    KOREADER_STORE.setSyncStats(true);
    SETTINGS.trackReadingStats = 1;
    const bool seed = strcmp(mode, "seed") == 0;
    const bool restore = strcmp(mode, "restore") == 0;
    ReadingStatsDateTime local;
    require(getCurrentLocalDailyReadingDateTime(local), "reset fixture clock");
    const uint32_t day = readingStatsDayIndex(local.date);
    const std::string cache = "/.crosspoint/epub_counter_test";
    require(Storage.ensureDirectoryExists(cache.c_str()), "reset book cache");
    if (strcmp(mode, "reset") == 0) {
      require(GlobalReadingStats::resetLocal() && BookReadingStats::remove(cache), "explicit local resets");
    }
    auto global = GlobalReadingStats::load();
    auto book = BookReadingStats::load(cache);
    if (seed) {  // adopted counters from firmware predating IDs
      global.counterId = {};
      book.counterId = {};
    }
    if (!restore) {
      global.totalReadingSeconds = book.totalReadingSeconds = seed ? 3600 : 60;
      global.totalSessions = book.sessionCount = seed ? 10 : 1;
      require(global.save() && book.save(cache), "save reset-aware snapshots");
      require(DailyReadingStats::record(day, seed ? 600 : 60) && DailyReadingStats::flush(), "save daily counters");
    }
    const bool oldServer = strcmp(mode, "old-server") == 0;
    const auto wanted = oldServer ? StatsUploadClient::Result::Unsupported : StatsUploadClient::Result::Ok;
    require(ReadingSyncUpload::globalStats() == wanted, "global reset-aware upload");
    if (oldServer) {
      DailyReadingStats::Counter counter;
      require(DailyReadingStats::read(day, counter) && counter.uploaded == 0, "old server retains daily outbox");
    } else {
      require(ReadingSyncUpload::globalStats() == wanted, "global retry");
      char device[32];
      require(StatsUploadClient::deviceId(device, sizeof(device)), "reset reader identity");
      std::vector<char> payload(StatsUploadPayload::CAPACITY);
      require(
          StatsUploadPayload::book(payload.data(), payload.size(), device, "0123456789abcdef0123456789abcdef", book),
          "reset book JSON");
      StatsUploadClient client;
      require(client.put("/api/v1/stats/counters/books", payload.data(), 1) == wanted, "reset book upload");
      require(client.put("/api/v1/stats/counters/books", payload.data(), 1) == wanted, "reset book retry");
    }
    LOG_INF("SMOKE", "Stats counter identity smoke passed: %s", mode);
    std::_Exit(0);
  }
  // Host-only buffers: firmware uses one reusable heap buffer in its activity.
  std::vector<char> payload(StatsUploadPayload::CAPACITY);
  GlobalReadingStats global;
  require(!GlobalReadingStats::loadForUpload(global), "missing global stats must not export zeros");
  global.totalSessions = 12;
  global.totalReadingSeconds = 3456;
  global.readingHistoryAnchorDay = 9769;
  global.readingHistoryBits[0] = 7;
  global.save();
  GlobalReadingStats loaded;
  require(GlobalReadingStats::loadForUpload(loaded) && loaded.totalReadingSeconds == 3456, "checked global read");
  require(StatsUploadPayload::global(payload.data(), payload.size(), "crossink-test", loaded), "global serialization");
  JsonDocument json;
  require(!deserializeJson(json, payload.data()), "global JSON");
  require(json["v"] == 5 && json["seconds"] == 3456 && json["tod"].size() == 4 && json["dow"].size() == 7,
          "global fields");
  require(strlen(json["history_b64"].as<const char*>()) == 124 &&
              std::strncmp(json["history_b64"].as<const char*>(), "BwAA", 4) == 0,
          "calendar bit order");
  // A v3 save interrupted halfway through its trailer must leave the other slot usable.
  ++global.totalSessions;
  require(global.save(), "create second global stats slot");
  FsFile torn;
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE + 5> partial{GlobalReadingStats::CURRENT_FILE_VERSION};
  require(Storage.openFileForWrite("SMOKE", StatsStorage::localPath("/.crosspoint/global_stats.bin").c_str(), torn) &&
              torn.write(partial.data(), partial.size()) == partial.size() && torn.close(),
          "truncate primary trailer");
  loaded = GlobalReadingStats::load();
  require(loaded.totalSessions == global.totalSessions && loaded.totalReadingSeconds == 3456,
          "recover backup after torn trailer");
  ++loaded.totalSessions;
  require(loaded.save() && GlobalReadingStats::load().totalSessions == loaded.totalSessions,
          "resume saving after recovery");
  // Every count at its maximum still fits the fixed request budget.
  global.totalSessions = global.totalReadingSeconds = global.totalPagesTurned = global.completedBooks = UINT32_MAX;
  global.timeOfDaySeconds.fill(UINT32_MAX);
  global.dayOfWeekSeconds.fill(UINT32_MAX);
  require(StatsUploadPayload::global(payload.data(), payload.size(), "crossink-test", global),
          "maximum global payload");
  require(!StatsUploadPayload::global(payload.data(), 20, "crossink-test", global), "buffer overflow rejected");
  require(!StatsUploadPayload::global(payload.data(), payload.size(), "bad\"id", global), "unsafe ID rejected");
  BookReadingStats book;
  book.startDate = {2000, 1, 1};
  book.finishedDate = {2099, 12, 31};
  book.isCompleted = true;
  book.sessionCount = UINT16_MAX;
  book.totalReadingSeconds = book.totalPagesTurned = book.estimatedTimeLeftSeconds = UINT32_MAX;
  book.timeOfDaySeconds.fill(UINT32_MAX);
  book.dayOfWeekSeconds.fill(UINT32_MAX);
  constexpr char DOCUMENT[] = "01b729228ded144417191bebc5a17445";
  require(StatsUploadPayload::book(payload.data(), payload.size(), "crossink-test", DOCUMENT, book),
          "maximum book payload");
  require(!deserializeJson(json, payload.data()), "book JSON");
  const auto item = json["items"][0];
  require(item["start_date"].as<uint64_t>() == 946684800ULL && item["finished_date"].as<uint64_t>() == 4102358400ULL,
          "dates remain UTC beyond 2038");
  require(item["completed"].is<bool>() && item["completed"] == true, "boolean fields");
  // Do not leave fixture counters in the normal app-flow smoke session.
  Storage.remove(StatsStorage::localPath("/.crosspoint/global_stats.bin").c_str());
  Storage.remove((StatsStorage::localPath("/.crosspoint/global_stats.bin") + ".bak").c_str());

  const char* server = std::getenv("CROSSINK_STATS_TEST_SERVER");
  if (server) {
    const char* user = std::getenv("CROSSINK_STATS_TEST_USER");
    const char* password = std::getenv("CROSSINK_STATS_TEST_PASSWORD");
    require(user && password, "explicit test credentials required");
    KOREADER_STORE.setCredentials(user, password);
    KOREADER_STORE.setServerUrl(server);
    if (std::getenv("CROSSINK_STATS_TEST_EMPTY_LIBRARY")) {
      // Stats sync is off by default; the fixture opts in explicitly.
      KOREADER_STORE.setSyncStats(true);
      global = {};
      global.save();
      if (std::getenv("CROSSINK_STATS_TEST_DAILY")) {
        require(DailyReadingStats::record(9769, 61) && DailyReadingStats::flush(), "manual daily fixture");
      }
      return;  // The main smoke state machine drives the actual activity.
    }
    StatsUploadClient client;
    char id[32];
    require(StatsUploadClient::deviceId(id, sizeof(id)), "simulator identity");
    if (const char* countText = std::getenv("CROSSINK_STATS_TEST_BATCH")) {
      const unsigned count = std::atoi(countText);
      require(count > 0 && count <= StatsUploadPayload::BookBatch::MAX_ITEMS, "batch count");
      std::vector<char> batchPayload(4096);
      StatsUploadPayload::BookBatch batch(batchPayload.data(), batchPayload.size());
      require(batch.begin(id), "batch start");
      for (unsigned i = 0; i < count; ++i) {
        char document[33];
        snprintf(document, sizeof(document), "%032x", i + 1);
        require(batch.add(document, BookReadingStats{}), "batch item");
      }
      require(batch.finish(), "batch finish");
      const char* expected = std::getenv("CROSSINK_STATS_TEST_ERROR");
      const auto wanted =
          expected ? static_cast<StatsUploadClient::Result>(std::atoi(expected)) : StatsUploadClient::Result::Ok;
      require(client.put("/api/v1/stats/books", batch.payload(), count) == wanted, "batch acknowledgment count");
      LOG_INF("SMOKE", "Stats upload transport smoke passed");
      std::_Exit(0);
    }
    if (const char* mode = std::getenv("CROSSINK_STATS_TEST_STEP")) {
      using Step = ReadingSyncUpload::GlobalStatsStep;
      using Result = StatsUploadClient::Result;
      SETTINGS.trackReadingStats = 1;
      KOREADER_STORE.setSyncStats(true);
      global = {};
      require(global.save(), "step fixture totals");
      if (strcmp(mode, "daily") == 0)
        require(DailyReadingStats::record(9769, 61) && DailyReadingStats::flush(), "step fixture daily history");
      if (strcmp(mode, "sessions") == 0 || strcmp(mode, "corrupt-session") == 0) {
        ReadingSessionJournal::Record record;
        strcpy(record.id, "0123456789abcdef0123456789abcdef");
        strcpy(record.document, DOCUMENT);
        record.startedAt = 1609502400;
        record.endedAt = 1609503000;
        record.seconds = 600;
        require(ReadingSessionJournal::save(record), "step fixture session");
        if (strcmp(mode, "corrupt-session") == 0)
          require(Storage.writeFile((StatsStorage::localPath("/.crosspoint/reading_sessions", true) +
                                     "/0123456789abcdef0123456789abcdef.bin")
                                        .c_str(),
                                    "bad"),
                  "step fixture corrupt session");
      }
      if (strcmp(mode, "empty-streak") == 0) {
        Storage.remove(StatsStorage::localPath("/.crosspoint/global_stats.bin").c_str());
        Storage.remove((StatsStorage::localPath("/.crosspoint/global_stats.bin") + ".bak").c_str());
      }
      KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
      const bool missingCounters = strncmp(mode, "missing-counters-", 17) == 0;
      Step failedStep = Step::Setup;  // Success must clear stale failure details.
      const bool skipped = strncmp(mode, "old-streak", 10) == 0 || strcmp(mode, "ok") == 0;
      const bool streak = strcmp(mode, "streak") == 0 || strcmp(mode, "empty-streak") == 0;
      const Step expectedStep = skipped                                          ? Step::None
                                : strcmp(mode, "totals") == 0 || missingCounters ? Step::Totals
                                : strcmp(mode, "daily") == 0                     ? Step::Daily
                                : streak                                         ? Step::Streak
                                                                                 : Step::Sessions;
      const auto expectedResult = skipped                                          ? Result::Ok
                                  : strcmp(mode, "totals") == 0 || missingCounters ? Result::Server
                                                                                   : Result::InvalidResponse;
      require(ReadingSyncUpload::globalStats(&failedStep) == expectedResult, "failed step result");
      require(failedStep == expectedStep, "failed step attribution");
      char status[128];
      const int httpCode = StatsUploadClient::lastHttpCode;
      const char* label =
          ReadingSyncUpload::globalStatusLabel(status, sizeof(status), expectedResult, failedStep, httpCode);
      if (!skipped && strcmp(mode, "corrupt-session") != 0)
        require(strstr(label, "HTTP ") != nullptr, "failed step retains HTTP status");
      const char* part = expectedStep == Step::Totals   ? "Totals"
                         : expectedStep == Step::Daily  ? "Daily history"
                         : expectedStep == Step::Streak ? "Reading Streak"
                                                        : "Sessions";
      require(skipped ? strcmp(label, "Done") == 0 : strstr(label, part) != nullptr, "failed step UI label");
      KOREADER_STORE.setSyncStats(false);
      require(
          strcmp(ReadingSyncUpload::globalStatusLabel(status, sizeof(status), expectedResult, failedStep), "OFF") == 0,
          "disabled stats keeps OFF label");
      KOREADER_STORE.setSyncStats(true);
      // The buffer-reusing overload must report local serialization failures too.
      require(global.save(), "restore totals for buffer test");
      require(ReadingSyncUpload::globalStats(client, payload.data(), 20, id, &failedStep) == Result::InvalidResponse &&
                  failedStep == Step::Totals,
              "small payload buffer attribution");
      SETTINGS.trackReadingStats = 0;
      require(ReadingSyncUpload::globalStats(&failedStep) == Result::Skipped && failedStep == Step::None,
              "skipped attempt clears stale failure");
      LOG_INF("SMOKE", "All-time stats failed-step smoke passed");
      std::_Exit(0);
    }
    if (const char* mode = std::getenv("CROSSINK_STATS_TEST_STREAK")) {
      SETTINGS.trackReadingStats = 1;
      SETTINGS.clockUtcOffsetQ = 48;
      KOREADER_STORE.setSyncStats(true);
      const char* fixtureDay = std::getenv("CROSSINK_STATS_TEST_STREAK_DAY");
      require(fixtureDay, "streak fixture day required");
      const uint32_t todayDay = std::strtoul(fixtureDay, nullptr, 10);
      ReadingStatsDate today;
      require(readingStatsDateFromDayIndex(todayDay, today), "streak fixture date");
      if (strcmp(mode, "empty") == 0) {
        require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "empty reader downloads streak");
        require(!GlobalReadingStats::loadForUpload(loaded), "download does not create local upload stats");
        require(loaded.currentReadingStreak(&today) == 3, "empty reader displays remote streak");
        std::_Exit(0);
      }
      global = {};
      global.totalReadingSeconds = 60;
      global.totalSessions = 1;
      global.readingHistoryAnchorDay = todayDay;
      global.readingHistoryBits[0] = 1;
      require(global.save(), "local streak fixture");
      require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "two-way stats sync");
      require(KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED, "download retains stats support");
      const bool old = strcmp(mode, "old-server") == 0;
      require(GlobalReadingStats::loadForUpload(loaded), "local snapshot after download");
      require(loaded.currentReadingStreak(&today) == (old ? 1 : 4), "combined streak displayed");
      require(loaded.readingHistoryBits[0] == 1 && loaded.totalReadingSeconds == 60, "download stays out of uploads");
      require(loaded.displayLongestReadingStreak() == (old ? 1 : 4), "combined longest streak");
      if (!old) {
        const auto second = strcmp(mode, "ok") == 0 ? ReadingSyncUpload::globalStats() : client.downloadStreak();
        const bool ok = strcmp(mode, "ok") == 0;
        const auto wanted = ok                            ? StatsUploadClient::Result::Ok
                            : strcmp(mode, "auth") == 0   ? StatsUploadClient::Result::Auth
                            : strcmp(mode, "server") == 0 ? StatsUploadClient::Result::Server
                                                          : StatsUploadClient::Result::InvalidResponse;
        require(second == wanted, "download error classification");
        require(loaded.currentReadingStreak(&today) == (ok ? 2 : 4), "replace calendar or retain on failure");
        // Local reading after a download extends the streak offline; uploads remain device-only.
        ReadingStatsDate tomorrow = today;
        addDaysToReadingStatsDate(tomorrow, 1);
        markReadingHistoryDay(loaded.readingHistoryAnchorDay, loaded.readingHistoryBits, todayDay + 1);
        require(loaded.currentReadingStreak(&tomorrow) == (ok ? 3 : 5), "offline streak extension");
        KOREADER_STORE.setSyncStats(false);
        require(loaded.currentReadingStreak(&today) == 1, "disabled stats ignores cached calendar");
        KOREADER_STORE.setSyncStats(true);
        KOREADER_STORE.setServerUrl(std::string(server) + "/other");
        require(loaded.currentReadingStreak(&today) == 1, "cache is scoped to server");
        KOREADER_STORE.setServerUrl(server);
        KOREADER_STORE.setCredentials("other-fixture", password);
        require(loaded.currentReadingStreak(&today) == 1, "cache is scoped to account");
      }
      LOG_INF("SMOKE", "Two-way streak download smoke passed");
      std::_Exit(0);
    }
    // Zero snapshots make an explicitly requested live test non-inflating.
    global = {};
    book = {};
    require(StatsUploadPayload::global(payload.data(), payload.size(), id, global), "test global payload");
    if (const char* mode = std::getenv("CROSSINK_STATS_TEST_SESSION")) {
      SETTINGS.trackReadingStats = 1;
      ReadingSessionJournal::Record record;
      strcpy(record.id, "0123456789abcdef0123456789abcdef");
      strcpy(record.document, "0123456789abcdef0123456789abcdef");
      record.startedAt = 1609502400;
      record.endedAt = 1609503000;
      record.seconds = 600;
      global.totalReadingSeconds = 600;
      global.save();
      require(ReadingSessionJournal::save(record), "session fixture");
      const auto first = ReadingSyncUpload::globalStats();
      const std::string path =
          StatsStorage::localPath("/.crosspoint/reading_sessions", true) + "/" + std::string(record.id) + ".bin";
      if (strcmp(mode, "ok") != 0) {
        const auto wanted = strcmp(mode, "old-server") == 0 ? StatsUploadClient::Result::Unsupported
                                                            : StatsUploadClient::Result::InvalidResponse;
        require(first == wanted, "session acknowledgment failure classification");
        require(Storage.exists(path.c_str()), "unacknowledged session retained");
        require(KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED, "legacy stats still supported");
        if (strcmp(mode, "old-server") == 0) std::_Exit(0);
      } else {
        require(first == StatsUploadClient::Result::Ok, "session first upload");
        require(!Storage.exists(path.c_str()), "acknowledged session removed");
        require(ReadingSessionJournal::save(record), "retry fixture");
      }
      require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "session retry");
      record.seconds = 1200;
      record.endedAt += 600;
      global.totalReadingSeconds = 1200;
      global.save();
      require(ReadingSessionJournal::save(record), "later session checkpoint");
      require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "later checkpoint upload");
      require(!Storage.exists(path.c_str()), "later checkpoint acknowledged");
      LOG_INF("SMOKE", "Reading session acknowledgment and retry smoke passed");
      std::_Exit(0);
    }
    const char* expected = std::getenv("CROSSINK_STATS_TEST_ERROR");
    auto result = client.put("/api/v1/stats/global", payload.data(), 0);
    if (expected) {
      const auto wanted = static_cast<StatsUploadClient::Result>(std::atoi(expected));
      require(result == wanted, "expected transport failure classification");
    } else {
      require(result == StatsUploadClient::Result::Ok, "global upload");
      if (std::getenv("CROSSINK_STATS_TEST_DAILY")) {
        SETTINGS.trackReadingStats = 1;
        global.save();
        require(DailyReadingStats::record(9769, 61), "daily fixture");
        if (std::getenv("CROSSINK_STATS_TEST_DAILY_NO_ACK")) {
          require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::InvalidResponse,
                  "old server daily ack missing");
          DailyReadingStats::Counter pending;
          require(DailyReadingStats::read(9769, pending) && pending.seconds == 61 && pending.uploaded == 0,
                  "old server preserves pending daily history");
          LOG_INF("SMOKE", "Missing daily acknowledgment keeps history retryable");
          std::_Exit(0);
        }
        require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "daily upload");
        require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "daily retry");
        DailyReadingStats::Counter counter;
        require(DailyReadingStats::read(9769, counter) && counter.seconds == 61 && counter.uploaded == 61, "daily ack");
        require(DailyReadingStats::record(9769, 9), "more daily reading");
        require(ReadingSyncUpload::globalStats() == StatsUploadClient::Result::Ok, "incremental daily upload");
        require(DailyReadingStats::read(9769, counter) && counter.seconds == 70 && counter.uploaded == 70,
                "incremental daily ack");
      }
      require(client.put("/api/v1/stats/global", payload.data(), 0) == StatsUploadClient::Result::Ok, "global retry");
      require(StatsUploadPayload::book(payload.data(), payload.size(), id, DOCUMENT, book), "test book payload");
      require(client.put("/api/v1/stats/books", payload.data(), 1) == StatsUploadClient::Result::Ok, "book upload");
      require(client.put("/api/v1/stats/books", payload.data(), 1) == StatsUploadClient::Result::Ok, "book retry");
    }
    LOG_INF("SMOKE", "Stats upload transport smoke passed");
    std::_Exit(0);
  }
  LOG_INF("SMOKE", "Stats upload serialization and storage smoke passed");
}
struct SyncDiagnosticsSmokeTest {
  static void verifyProgressDiscovery(GfxRenderer& renderer, MappedInputManager& input) {
    const std::string linked = "/scope/linked.epub";
    const std::string older = "/scope/older.epub";
    const std::string oldHash = "/old-hash.epub";
    const std::string restored = "/restored.epub";
    const auto fixture = [](const std::string& path) {
      require(Storage.writeFile(path.c_str(), "fixture"), "create EPUB filename");
      Epub epub(path, "/.crosspoint");
      epub.setupCacheDir();
      require(EpubReaderUtils::saveProgress(epub, 1, 2, 10), "save progress and book path");
      return epub.getCachePath();
    };
    require(Storage.ensureDirectoryExists("/scope"), "create progress test folder");
    const auto linkedCache = fixture(linked);
    const auto olderCache = fixture(older);
    require(Storage.remove((olderCache + "/progress-path.bin").c_str()), "remove path for older cache");
    const auto currentHashCache = fixture(oldHash);
    require(Storage.remove((currentHashCache + "/progress-path.bin").c_str()) &&
                Storage.rename(currentHashCache.c_str(),
                               ("/.crosspoint/epub_" + std::to_string(Epub::legacyCachePathHash(oldHash))).c_str()),
            "seed legacy cache directory hash");
    require(Storage.writeFile("/scope/unread.epub", "fixture"), "unread EPUB");
    Epub unread("/scope/unread.epub", "/.crosspoint");
    unread.setupCacheDir();
    Epub missing("/missing.epub", "/.crosspoint");
    missing.setupCacheDir();
    require(EpubReaderUtils::saveProgress(missing, 0, 1, 10), "cache for removed EPUB");
    for (const auto& path : {std::string("/orphan.epub"), restored}) {
      const auto cache = fixture(path);
      require(Storage.remove(path.c_str()) && Storage.remove((cache + "/progress-path.bin").c_str()),
              "orphan legacy cache");
    }

    KOREADER_STORE.setSyncStats(false);
    KOREADER_STORE.setSyncClippings(false);
    const auto native = [&](const std::string& root) {
      StatsUploadActivity sync(renderer, input, root);
      sync.startLegacyWalk();
      require(sync.progressBooks != nullptr && sync.folderBooks == nullptr, "native progress-only walk uses caches");
      std::vector<std::string> books;
      for (unsigned i = 0; i < 16000; ++i) {
        std::string path;
        const auto next = sync.nextBook(path);
        require(next != StatsUploadActivity::NextBook::Error, "native progress discovery succeeds");
        if (next == StatsUploadActivity::NextBook::Book) books.push_back(std::move(path));
        if (next == StatsUploadActivity::NextBook::Done) {
          std::sort(books.begin(), books.end());
          return books;
        }
      }
      require(false, "native enumeration terminates");
      return books;
    };
    const auto usb = [&] {
      std::vector<std::string> books;
      JsonDocument request;
      request["op"] = "books";
      for (uint32_t cursor = 0; cursor < 16000; ++cursor) {
        request["cursor"] = cursor;
        JsonDocument response;
        UsbProgressSync::handle(request.as<JsonVariantConst>(), response, renderer);
        require(response["error"].isNull(), "USB progress discovery succeeds");
        for (const auto item : response["books"].as<JsonArrayConst>()) books.emplace_back(item.as<const char*>());
        if (response["done"].as<bool>()) {
          std::sort(books.begin(), books.end());
          return books;
        }
        require(response["next"].as<uint32_t>() == cursor + 1, "USB listing advances");
      }
      require(false, "USB listing terminates");
      return books;
    };
    const auto blocked = olderCache + "/progress-path.bin.tmp";
    require(
        Storage.ensureDirectoryExists(blocked.c_str()) && Storage.writeFile((blocked + "/blocker").c_str(), "fixture"),
        "block a legacy path write");
    require(native("/scope") == std::vector<std::string>{linked, older},
            "legacy write failure still returns saved progress");
    require(!Storage.exists((olderCache + "/progress-path.bin").c_str()), "fixture prevented saving the association");
    require(native("/scope") == std::vector<std::string>{linked, older},
            "failed path writes are retried on later scans");
    require(Storage.removeDir(blocked.c_str()), "let path recovery succeed");
    require(native("/scope") == std::vector<std::string>{linked, older}, "folder sync includes older progress once");
    require(Storage.exists((olderCache + "/progress-path.bin").c_str()), "native walk backfills older cache");
    require(usb() == std::vector<std::string>{oldHash, linked, older}, "USB sync includes both cache formats");
    require(Storage.exists((currentHashCache + "/progress-path.bin").c_str()), "USB walk backfills old hash cache");

    require(Storage.writeFile(restored.c_str(), "fixture"), "restore an EPUB from an older cache");
    library::invalidateLibraryIndex();
    require(usb() == std::vector<std::string>{oldHash, restored, linked, older},
            "library changes make a previous orphan eligible for legacy lookup again");

    const std::string moved = "/scope/moved.epub";
    require(
        BookMoveUtils::renameFilePreservingBookState(linked, moved) == BookMoveUtils::RenameMigrationResult::Success,
        "rename a book with a path record");
    require(!Storage.exists(linkedCache.c_str()), "rename moves the old cache");
    require(native("/") == std::vector<std::string>{oldHash, restored, moved, older},
            "rename invalidates the previous orphan lookup");
    // A card walk would hit the iterator's depth limit after migration.
    std::string deep;
    for (int i = 0; i < 18; ++i) deep += "/deep";
    require(Storage.ensureDirectoryExists(deep.c_str()), "create card walk tripwire");
    require(native("/") == std::vector<std::string>{oldHash, restored, moved, older},
            "native migrated sync skips card walk despite an orphan");
    require(usb() == std::vector<std::string>{oldHash, restored, moved, older},
            "USB migrated sync skips card walk despite an orphan");
    LOG_INF("SMOKE", "Cache-first progress discovery passed");
    std::_Exit(0);
  }

  static void run(GfxRenderer& renderer, MappedInputManager& input) {
    const char* directory = std::getenv("CROSSINK_SYNC_DIAGNOSTICS_CAPTURES");
    std::filesystem::create_directories(directory);
    KOREADER_STORE.setSyncStats(true);
    KOREADER_STORE.setSyncClippings(true);
    KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
    const auto capture = [&](const char* name, Activity& activity) {
      RenderLock lock;
      activity.render(std::move(lock));
      const auto path = std::filesystem::path(directory) / (std::string(name) + ".pgm");
      FILE* file = fopen(path.c_str(), "wb");
      require(file != nullptr, "create diagnostic screenshot");
      fprintf(file, "P5\n%d %d\n255\n", renderer.getScreenWidth(), renderer.getScreenHeight());
      for (int y = 0; y < renderer.getScreenHeight(); ++y)
        for (int x = 0; x < renderer.getScreenWidth(); ++x) fputc(renderer.isPixelBlack(x, y) ? 0 : 255, file);
      require(fclose(file) == 0, "close diagnostic screenshot");
    };
    SDL_setenv("CROSSINK_FAIL_STORAGE_SYNC", "1", 1);
    require(!silentRestartToNetwork(NetworkBootTarget::KOREADER_SYNC), "failed flush cancels network restart");
    require(APP_STATE.hasPendingAlert.load(), "failed flush queues a dismissible storage alert");
    armSilentRestartReaderPageBuild("/books/Example.epub", 1, 3, false);
    {
      RenderLock held;
      require(!silentRestartToReader(), "failed flush borrows an existing render lock and cancels restart");
    }
    uint16_t stagedSpine = 0, stagedPage = 0;
    bool stagedAutomatic = false;
    require(!consumeSilentRestartReaderPageBuild("/books/Example.epub", stagedSpine, stagedPage, stagedAutomatic),
            "failed restart clears staged page-build token");
    SDL_setenv("CROSSINK_FAIL_STORAGE_SYNC", "0", 1);
    APP_STATE.hasPendingAlert.store(false);

    auto history = GlobalReadingStats::load();
    history.totalReadingSeconds = 97000;
    require(history.save() && backupGlobalStats(false), "prepare recovery snapshot");
    const auto historyPath = StatsStorage::localPath("/.crosspoint/global_stats.bin");
    for (const auto& slot : {historyPath, historyPath + ".bak"}) {
      if (!Storage.exists(slot.c_str())) continue;
      auto damaged = Storage.open(slot.c_str(), O_RDWR);
      require(bool(damaged) && damaged.seekSet(damaged.fileSize() - 1), "open stats for corruption fixture");
      uint8_t last = 0;
      require(damaged.read(&last, 1) == 1 && damaged.seekSet(damaged.fileSize() - 1), "read checksum fixture");
      last ^= 1;
      require(damaged.write(&last, 1) == 1 && damaged.close(), "corrupt stats checksum fixture");
    }

    for (const auto orientation : {GfxRenderer::Orientation::Portrait, GfxRenderer::Orientation::LandscapeClockwise}) {
      renderer.setOrientation(orientation);
      const bool portrait = orientation == GfxRenderer::Orientation::Portrait;
      if (portrait) {
        BackupStatsActivity recovery(renderer, input);
        recovery.onEnter();
        require(recovery.recovering && recovery.state == BackupStatsActivity::WARNING,
                "unreadable history offers a verified backup instead of replacing it");
        capture("stats-recovery-confirm", recovery);
        recovery.runBackup();
        require(recovery.state == BackupStatsActivity::SUCCESS && !GlobalReadingStats::recoveryRequired(),
                "confirmed backup recovery succeeds");
        require(GlobalReadingStats::load().counterId.bytes == history.counterId.bytes,
                "backup recovery keeps original counter identity");
        capture("stats-recovery-done", recovery);
      }
      auto bulk = std::make_unique<StatsUploadActivity>(renderer, input, "", StatsUploadActivity::Scope::ReadingStats);
      bulk->statsFailed = 1;
      bulk->globalResult = StatsUploadClient::Result::Server;
      bulk->globalFailedStep = ReadingSyncUpload::GlobalStatsStep::Totals;
      bulk->globalHttpCode = 503;
      bulk->failBook("/books/Example.epub", SyncPending::STATS_BIT, "Server error (HTTP 503)");
      bulk->failBook("/books/Example.epub", SyncPending::PROGRESS_BIT, "Authentication Failed (HTTP 401)");
      require(bulk->failed == 2 && bulk->failures.size() == 1 && bulk->omittedFailureDetails == 0,
              "one failed book merges multiple attempts without claiming extra books");
      require(bulk->failures[0].parts == (SyncPending::STATS_BIT | SyncPending::PROGRESS_BIT) &&
                  bulk->failures[0].reason.find("HTTP 401") != std::string::npos &&
                  bulk->failures[0].reason.find("HTTP 503") != std::string::npos,
              "merged failure preserves component masks and HTTP reasons");
      bulk->finishUpload();
      require(bulk->message == tr(STR_SYNC_FAILED_MSG), "failed bulk cannot say Done");
      capture(portrait ? "bulk-failed-portrait" : "bulk-failed-landscape", *bulk);
      auto library = std::make_unique<StatsUploadActivity>(renderer, input);
      library->clippingPullStarted = true;
      library->statsFailed = library->clippingsFailed = 1;
      library->globalResult = StatsUploadClient::Result::Server;
      library->globalFailedStep = ReadingSyncUpload::GlobalStatsStep::Totals;
      library->globalHttpCode = 503;
      library->failBook("/books/Example.epub", SyncPending::PROGRESS_BIT, "Authentication Failed (HTTP 401)");
      library->failBook("/books/Example.epub", SyncPending::CLIPPINGS_BIT, "Server error (HTTP 500)");
      library->finishUpload();
      capture(portrait ? "library-failed-portrait" : "library-failed-landscape", *library);
      require(!library->failuresBelow, "full-library failure row fits available height");
      auto reader = std::make_unique<KOReaderSyncActivity>(renderer, input, "/books/Example.epub",
                                                           DocumentMatchMethod::FILENAME, SETTINGS.orientation);
      reader->state = KOReaderSyncActivity::SYNC_FAILED;
      reader->extrasAttempted = true;
      reader->progressSucceeded = true;
      reader->globalStatsResult = StatsUploadClient::Result::Server;
      reader->globalStatsFailedStep = ReadingSyncUpload::GlobalStatsStep::Totals;
      reader->globalStatsHttpCode = 503;
      reader->extrasResult.stats = StatsUploadClient::Result::InvalidResponse;
      reader->extrasResult.statsHttpCode = 200;
      reader->extrasResult.clippings = StatsUploadClient::Result::Ok;
      capture(portrait ? "reader-failed-portrait" : "reader-failed-landscape", *reader);
      reader->globalStatsResult = StatsUploadClient::Result::Network;
      reader->globalStatsFailedStep = ReadingSyncUpload::GlobalStatsStep::Sessions;
      reader->globalStatsHttpCode = -1;
      reader->extrasResult.stats = reader->extrasResult.clippings = StatsUploadClient::Result::Network;
      reader->extrasResult.statsHttpCode = reader->extrasResult.clippingsHttpCode = -1;
      capture(portrait ? "position-synced-portrait" : "position-synced-landscape", *reader);
      reader->progressSucceeded = false;
      reader->progressFailed = true;
      reader->extrasAttempted = false;
      reader->statusMessage = tr(STR_CONNECTION_FAILED);
      capture(portrait ? "position-failed-portrait" : "position-failed-landscape", *reader);
      reader->epub = std::make_shared<Epub>("/books/Example.epub", "/.crosspoint");
      require(!reader->ensureLocalProgressLoaded() && reader->localPositionUnavailable,
              "unreadable local position is not mapped to chapter one");
      reader->performUpload();
      require(reader->state == KOReaderSyncActivity::SYNC_FAILED,
              "missing local position cannot enter the upload path");
      reader->remotePosition.valid = true;
      reader->remotePosition.spineIndex = 0;
      reader->remotePosition.pageNumber = 12;
      reader->remoteProgress.percentage = 0.4f;
      reader->remoteProgress.device = "Other reader";
      reader->finishComparison(KOReaderSyncClient::OK);
      require(reader->state == KOReaderSyncActivity::SHOWING_RESULT && reader->selectedOption == 0,
              "server progress offers explicit download-only recovery");
      capture(portrait ? "progress-recovery-portrait" : "progress-recovery-landscape", *reader);
      // Each independent failure category must reject the success heading.
      for (unsigned kind = 0; kind < 4; ++kind) {
        bulk->failed = kind == 0;
        bulk->statsFailed = kind == 1;
        bulk->clippingsFailed = kind == 2;
        bulk->globalResult = kind == 3 ? StatsUploadClient::Result::InvalidResponse : StatsUploadClient::Result::Ok;
        bulk->finishUpload();
        require(bulk->message == tr(STR_SYNC_FAILED_MSG), "independent failure keeps failure heading");
      }
      bulk->failed = bulk->statsFailed = bulk->clippingsFailed = 0;
      bulk->globalResult = StatsUploadClient::Result::Ok;
      bulk->finishUpload();
      require(bulk->message == tr(STR_DONE), "successful sync still says Done");
    }
    auto capped = std::make_unique<StatsUploadActivity>(renderer, input);
    capped->failures.reserve(StatsUploadActivity::MAX_FAILURE_DETAILS);
    for (unsigned i = 0; i < 25; ++i)
      capped->failBook("/folder/" + std::to_string(i) + "/same.epub", SyncPending::PROGRESS_BIT, "HTTP 401");
    capped->failBook("/folder/0/same.epub", SyncPending::CLIPPINGS_BIT, "HTTP 500");
    require(capped->failed == 26 && capped->failures.size() == 20 && capped->omittedFailureDetails == 5,
            "capped details count omitted attempts independently of merged rows");
    LOG_INF("SMOKE", "Sync diagnostic result screens passed");
    std::_Exit(0);
  }
};
void verifySyncDiagnosticsScreens(GfxRenderer& renderer, MappedInputManager& input) {
  if (std::getenv("CROSSINK_PROGRESS_DISCOVERY_TEST")) {
    SyncDiagnosticsSmokeTest::verifyProgressDiscovery(renderer, input);
  }
  SyncDiagnosticsSmokeTest::run(renderer, input);
}
#endif
