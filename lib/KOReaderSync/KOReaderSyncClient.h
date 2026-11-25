#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * Optional document metadata sent alongside progress sync requests.
 * Mirrors the metadata object added in KOReader PR #15306.
 * The official sync server ignores this field; custom servers may use it.
 */
class Epub;
struct KOReaderMetadata {
  void loadExtended(Epub& epub);

  std::string filename;  // e.g. "my_book.epub"
  std::string title;     // Document title from EPUB metadata
  std::string authors;   // Author(s) from EPUB metadata
  std::string isbn, asin, series;
  std::optional<float> seriesIndex;
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (see crosspoint-sync docs/API.md).
 * It is transmitted only for profiles that support rich progress. Layout
 * coordinates are hints; the standard XPath remains the content anchor.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000 (authoritative)
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage;                              // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp;                             // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Optional document metadata
  std::optional<KOReaderRichPosition> position;  // Optional rich position (crosspoint-sync servers only)
};

/**
 * One entry of the CrossPoint Sync progress change feed: a document's newest
 * position, with every hash it is known by.
 */
struct KOReaderProgressChange {
  // A merged document rarely has more than a couple of aliases; the matching
  // hash can be any of them, so keep enough.
  static constexpr size_t MAX_ALIASES = 8;
  std::string document;  // canonical hash
  std::string aliases[MAX_ALIASES];
  uint8_t aliasCount = 0;
  float percentage = 0;
  std::string progress;  // KOReader xpath
  std::optional<KOReaderRichPosition> position;
  std::string device;
  int64_t timestamp = 0;
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  // Shared admission budget for progress, stats, and clipping connections.
  static constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
#if defined(WOLFSSL_HAVE_SP_RSA)
  // SP RSA scratch fits below SecureClient's 16,704-byte full-record buffer.
  // Allow allocator overhead without rejecting a usable 18 KB free block.
  static constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 18000;
#else
  static constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;
#endif

  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    INVALID_AUTH_RESPONSE,
    LOW_MEMORY,
    USER_EXISTS,
    FEED_UNSUPPORTED,   // stock KOSync or an older CrossPoint Sync: no change feed
    BATCH_UNSUPPORTED,  // no PUT /api/v1/progress/batch: send one PUT per book
    RESPONSE_TOO_LARGE,
    STORAGE_ERROR
  };

  // Progress positions per batch request: each item is a few hundred bytes and
  // the whole body is held in RAM during the TLS request.
  static constexpr size_t MAX_PROGRESS_BATCH = 8;

  struct ChangesPage {
    uint32_t cursor = 0;  // where the next page starts
    bool more = false;
    uint16_t count = 0;  // changes passed to the callback
  };
  // Called once per change in a page, in feed order. Return false to stop.
  using ChangeCallback = bool (*)(const KOReaderProgressChange& change, void* context);

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Register a new account on the sync server using the stored credentials
   * (POST /users/create with the MD5 auth key — the server never sees the
   * plain password).
   * @return OK on success, USER_EXISTS if the username is taken
   */
  static Error createUser();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * PUT /api/v1/progress/batch (CrossPoint Sync): up to MAX_PROGRESS_BATCH
   * positions in one request, all accepted or none. Consumes `items` (cleared
   * before the request, so only the serialized body is alive during TLS).
   * @return OK, BATCH_UNSUPPORTED on servers without the route, or an error
   */
  static Error updateProgressBatch(std::vector<KOReaderProgress>& items);

  /**
   * GET /api/v1/progress/changes?since=&limit=&device=: progress changed since
   * a cursor, leaving out documents whose newest position is this reader's.
   * `limit` 0 only reads the current cursor. Responses are staged on SD with
   * a 16 KiB wire limit; the server may exceed its soft 8 KB page budget for
   * one large change. Resource failures retry smaller pages at the same cursor.
   * Parsing finishes before each change is handed to `callback`.
   */
  static Error getProgressChanges(uint32_t since, uint8_t limit, ChangesPage& page, ChangeCallback callback,
                                  void* context);

  /**
   * Get human-readable error message.
   */
  static std::string errorString(Error error);

  // This reader's own device id ("crossink-<mac>"), shared by progress, stats
  // and clippings uploads. Each reader gets its own server progress row, so the
  // change feed can leave out a reader's own writes without hiding another
  // CrossInk reader's progress.
  static bool readerId(char* out, size_t capacity);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
