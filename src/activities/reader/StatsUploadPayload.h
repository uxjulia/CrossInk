#pragma once

#include <cstddef>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// One snapshot at a time, with no JSON tree or library-sized allocation.
namespace StatsUploadPayload {
constexpr size_t CAPACITY = 1536;
bool global(char* out, size_t capacity, const char* deviceId, const GlobalReadingStats& stats,
            uint32_t dailyDay = UINT32_MAX, uint32_t dailySeconds = 0, const StatsCounterId& dailyId = {});
bool book(char* out, size_t capacity, const char* deviceId, const char* document, const BookReadingStats& stats);

// Several books in one PUT /api/v1/stats/books request: each request costs the
// device a TLS handshake. Writes into the caller's fixed buffer; an item that
// does not fit is rolled back so the caller can send the batch and start again.
class BookBatch {
 public:
  // The server accepts at most 20 items per request (MAX_BOOK_BATCH).
  static constexpr size_t MAX_ITEMS = 20;
  BookBatch(char* out, size_t capacity) : out(out), capacity(capacity) {}
  bool begin(const char* deviceId);
  // False when the item does not fit (buffer or MAX_ITEMS) or is invalid; the batch is unchanged.
  bool add(const char* document, const BookReadingStats& stats);
  // Closes the JSON. Call once before sending; begin() starts the next batch.
  bool finish();
  size_t count() const { return items; }
  const char* payload() const { return out; }

 private:
  char* out;
  size_t capacity;
  size_t length = 0;
  size_t items = 0;
};
}  // namespace StatsUploadPayload
