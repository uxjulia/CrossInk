#ifdef SIMULATOR
#include "StatsUploadSmokeTest.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/reader/DailyReadingStats.h"
#include "activities/reader/ReadingSessionJournal.h"
#include "activities/reader/StatsUploadPayload.h"
#include "network/ReadingSyncUpload.h"
#include "network/StatsUploadClient.h"

namespace {
void require(bool ok, const char* what) {
  if (!ok) {
    LOG_ERR("SMOKE", "Stats upload contract failed: %s", what);
    std::_Exit(2);
  }
}
}  // namespace

void verifyStatsUploadContract() {
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
  Storage.remove("/.crosspoint/global_stats.bin");
  Storage.remove("/.crosspoint/global_stats.bin.bak");

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
      const std::string path = "/.crosspoint/reading_sessions/" + std::string(record.id) + ".bin";
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
    auto result = client.put("/api/v1/stats/global", payload.data(), false);
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
      require(client.put("/api/v1/stats/global", payload.data(), false) == StatsUploadClient::Result::Ok,
              "global retry");
      require(StatsUploadPayload::book(payload.data(), payload.size(), id, DOCUMENT, book), "test book payload");
      require(client.put("/api/v1/stats/books", payload.data(), true) == StatsUploadClient::Result::Ok, "book upload");
      require(client.put("/api/v1/stats/books", payload.data(), true) == StatsUploadClient::Result::Ok, "book retry");
    }
    LOG_INF("SMOKE", "Stats upload transport smoke passed");
    std::_Exit(0);
  }
  LOG_INF("SMOKE", "Stats upload serialization and storage smoke passed");
}
#endif
