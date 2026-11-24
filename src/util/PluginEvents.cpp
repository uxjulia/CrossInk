#include "PluginEvents.h"

#include <ArduinoJson.h>
#include <FilePublication.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryFileTypes.h>
#include <Logging.h>
#include <Memory.h>
#ifndef SIMULATOR
#include <esp_random.h>
#else
#include <random>
static uint32_t esp_random() { return std::random_device{}(); }
#endif
#include <DownloadFileSwap.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <time.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "PluginEventOutbox.h"
#include "PluginHttp.h"
#include "PluginLocations.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "network/ProtectedPaths.h"

namespace {

constexpr const char* EVENT_NAMES[] = {"reader.open", "reader.exit", "reader.session", "book.downloaded",
                                       "sleep.enter"};
static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == static_cast<size_t>(pluginevents::Event::COUNT),
              "event name table out of sync");

constexpr const char* OUTBOX_NAME = PluginEventOutbox::NAME;
constexpr size_t MAX_OUTBOX_BYTES = PluginEventOutbox::MAX_OUTBOX_BYTES;
constexpr size_t MAX_EVENT_LINE = PluginEventOutbox::MAX_EVENT_LINE;
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
// Event handler responses are acknowledgements, not content.
constexpr size_t MAX_EVENT_RESPONSE = 8 * 1024;

// Static subscription table: one slot per installed plugin that declares an
// "events" section. Rebuilt by refreshSubscriptions(); sized for a full
// plugin list screen, not a marketplace.
constexpr size_t MAX_EVENT_PLUGINS = 8;
struct Subscriber {
  char name[81] = {0};                                    // plugin folder name; "" = empty slot
  char dir[PluginEventOutbox::DIRECTORY_CAPACITY] = {0};  // "<root>/<name>"
  uint8_t mask = 0;                                       // bit per Event
  uint8_t connectMask = 0;                                // events whose handler declares "connect": true
};
Subscriber subscribers[MAX_EVENT_PLUGINS];
std::mutex outboxMutex;

uint8_t eventBit(const pluginevents::Event e) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(e)); }

int eventFromName(const char* name) {
  for (size_t i = 0; i < static_cast<size_t>(pluginevents::Event::COUNT); i++) {
    if (strcmp(EVENT_NAMES[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

std::string outboxPath(const Subscriber& sub) { return std::string(sub.dir) + OUTBOX_NAME; }

bool writeOutbox(const std::string& path, const char* bytes, size_t size) {
  const std::string part = path + ".part";
  HalFile file = Storage.open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  const bool written = file && file.write(reinterpret_cast<const uint8_t*>(bytes), size) == size && file.sync();
  const bool closed = file.close();
  if (!written || !closed || !DownloadFileSwap::publish(path)) {
    LOG_ERR("PEVT", "Outbox write failed: %s", path.c_str());
    return false;
  }
  return true;
}

}  // namespace

namespace pluginevents {

void refreshSubscriptions() {
  std::lock_guard<std::mutex> guard(outboxMutex);
  for (auto& sub : subscribers) sub = Subscriber{};

  size_t slot = 0;
  for (const auto& entry : PluginLocations::scanPlugins()) {
    if (!entry.hasDevice) continue;
    if (slot >= MAX_EVENT_PLUGINS) {
      LOG_ERR("PEVT", "subscription table full; ignoring %s", entry.name.c_str());
      break;
    }
    // The fixed-size table cannot hold these: a truncated name or folder would
    // point emit() and drain() at a different outbox than the plugin's own.
    if (entry.name.size() >= sizeof(Subscriber::name) || entry.dir.size() >= sizeof(Subscriber::dir)) {
      LOG_ERR("PEVT", "plugin name/path too long for events; ignoring %s", entry.name.c_str());
      continue;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", entry.dir + "/device.json", MAX_MANIFEST_SIZE, raw)) continue;
    // Filtered parse: only the events section, so a big manifest costs a few
    // hundred bytes here instead of a full document.
    JsonDocument filter;
    filter["events"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    uint8_t mask = 0;
    uint8_t connectMask = 0;
    for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
      const int e = eventFromName(kv.key().c_str());
      if (e < 0) {
        LOG_DBG("PEVT", "%s: unknown event '%s' ignored", entry.name.c_str(), kv.key().c_str());
        continue;
      }
      mask |= static_cast<uint8_t>(1u << e);
      if (kv.value()["connect"] | false) connectMask |= static_cast<uint8_t>(1u << e);
    }
    // sleep.enter exists to act before the chip powers down (sleep image,
    // pre-sleep sync), so subscribing implies "connect": true; requiring the
    // flag would make the common case silently defer to the next session.
    if (mask & eventBit(Event::SleepEnter)) connectMask |= eventBit(Event::SleepEnter);
    if (mask == 0) continue;
    Subscriber& sub = subscribers[slot++];
    // Lengths checked above, so these copy whole strings.
    strncpy(sub.name, entry.name.c_str(), sizeof(sub.name) - 1);
    strncpy(sub.dir, entry.dir.c_str(), sizeof(sub.dir) - 1);
    sub.mask = mask;
    sub.connectMask = connectMask;
    LOG_DBG("PEVT", "%s subscribes mask=0x%02x", sub.name, sub.mask);
  }
}

bool anySubscriber(const Event e) {
  if (!SETTINGS.pluginsEnabled) return false;
  std::lock_guard<std::mutex> guard(outboxMutex);
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && (sub.mask & eventBit(e))) return true;
  }
  return false;
}

uint8_t subscriptionMask(const char* plugin) {
  if (!SETTINGS.pluginsEnabled) return 0;
  std::lock_guard<std::mutex> guard(outboxMutex);
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && strcmp(sub.name, plugin) == 0) return sub.mask;
  }
  return 0;
}

bool wantsConnectAny() {
  if (!SETTINGS.pluginsEnabled) return false;
  std::lock_guard<std::mutex> guard(outboxMutex);
  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || sub.connectMask == 0) continue;
    std::string raw;
    if (!Storage.readFileToString("PEVT", outboxPath(sub), MAX_OUTBOX_BYTES + MAX_EVENT_LINE, raw)) continue;
    for (size_t i = 0; i < static_cast<size_t>(Event::COUNT); i++) {
      if (!(sub.connectMask & eventBit(static_cast<Event>(i)))) continue;
      char eventField[48];
      snprintf(eventField, sizeof(eventField), "\"e\":\"%s\"", EVENT_NAMES[i]);
      if (raw.find(eventField) != std::string::npos) return true;
    }
  }
  return false;
}

void emit(const Event e, const Var* vars, const size_t varCount) {
  if (!anySubscriber(e)) return;

  std::lock_guard<std::mutex> guard(outboxMutex);
  HeapByteBuffer line;
  size_t lineSize = 0;
  {
    // One line: {"e":"reader.exit","id":"3fa9c21b-7","ts":1734212345,"vars":{"book":"...","percent":"74"}}
    JsonDocument doc;
    doc["e"] = EVENT_NAMES[static_cast<size_t>(e)];
    // Unique id for server-side dedupe of at-least-once delivery.
    static const uint32_t bootNonce = esp_random();
    static uint32_t seq = 0;
    char id[24];
    snprintf(id, sizeof(id), "%08lx-%lu", static_cast<unsigned long>(bootNonce), static_cast<unsigned long>(++seq));
    doc["id"] = id;
    // Best-effort unix time: 0 when the clock was never set (no RTC, no NTP yet).
    doc["ts"] = static_cast<long long>(trustedtime::trustedNow());
    JsonObject varsObj = doc["vars"].to<JsonObject>();
    for (size_t i = 0; i < varCount; i++) {
      varsObj[vars[i].key] = vars[i].value;
    }
    if (doc.overflowed()) {
      LOG_ERR("PEVT", "Cannot allocate plugin event JSON");
      return;
    }
    const size_t bytes = measureJson(doc);
    if (bytes >= MAX_EVENT_LINE) {
      LOG_ERR("PEVT", "Event line too large (%u); dropped", static_cast<unsigned>(bytes));
      return;
    }
    // Exact, fallible allocation once for all subscribers. A maximum 2 KiB
    // event does not belong on the loop stack. Release the JSON pool before I/O.
    lineSize = bytes + 1;
    line = makeDefaultByteBufferNoThrow(lineSize);
    if (!line || serializeJson(doc, reinterpret_cast<char*>(line.get()), lineSize) != bytes) {
      LOG_ERR("PEVT", "Cannot serialize plugin event (%u bytes); dropped", static_cast<unsigned>(lineSize));
      return;
    }
    line[bytes] = '\n';
  }

  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || !(sub.mask & eventBit(e))) continue;
    PluginEventOutbox::append(sub.dir, reinterpret_cast<const char*>(line.get()), lineSize);
  }
}

namespace {

// The manifest subset a drain needs: the handlers plus the token/config/auth
// vocabulary shared with the catalog browser.
struct DrainManifest {
  std::string tokenFile, tokenPath, configFile;
  std::string authType, authTokenPath;
  pluginhttp::RequestSpec authReq;
  struct Handler {
    int event = -1;
    pluginhttp::RequestSpec req;
    std::string toast;
    // "download" variant: the response streams to `dest` on SD (e.g. a fresh
    // sleep image) instead of being read as an acknowledgement.
    std::string dest;
    bool isDownload() const { return !dest.empty(); }
  };
  std::vector<Handler> handlers;
  bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
};

// False only when device.json cannot be read or parsed; a manifest that parses
// but declares no runnable handlers returns true with out.handlers empty.
bool loadDrainManifest(const Subscriber& sub, DrainManifest& out) {
  std::string raw;
  if (!Storage.readFileToString("PEVT", std::string(sub.dir) + "/device.json", MAX_MANIFEST_SIZE, raw)) return false;
  // Filtered parse: the drain runs at the heap-worst moments (sleep entry,
  // web session), so only the sections it reads are materialized.
  JsonDocument filter;
  filter["token"] = true;
  filter["config"] = true;
  filter["auth"] = true;
  filter["events"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;

  out.tokenFile = pluginhttp::inPluginDir(sub.dir, doc["token"]["file"] | "");
  out.tokenPath = doc["token"]["path"] | "token";
  out.configFile = pluginhttp::inPluginDir(sub.dir, doc["config"]["file"] | "");
  JsonVariantConst auth = doc["auth"];
  out.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", out.authReq);
  out.authTokenPath = auth["token_path"] | "access_token";

  out.handlers.reserve(2);
  for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
    const int e = eventFromName(kv.key().c_str());
    if (e < 0) continue;
    DrainManifest::Handler h;
    h.event = e;
    JsonVariantConst dl = kv.value()["download"];
    if (dl["url"].as<const char*>()) {
      pluginhttp::readRequest(dl, "GET", h.req);
      h.dest = dl["dest"] | "";
      if (h.dest.empty()) continue;  // a download without a destination is meaningless
    } else {
      pluginhttp::readRequest(kv.value()["request"], "POST", h.req);
    }
    h.toast = kv.value()["toast"] | "";
    if (!h.req.url.empty()) out.handlers.push_back(std::move(h));
  }
  return true;
}

// {token}, {cfg.*}, {meta.*}, and {event.*} from the queued line's vars
// object. `config` and `meta` hold pre-built patterns ("{cfg.KEY}" /
// "{meta.KEY}") so the keys are not re-concatenated for every template.
std::string drainSubstituted(std::string tpl, const std::string& token, const pluginhttp::Headers& config,
                             const pluginhttp::Headers& meta, JsonVariantConst vars, const long long ts,
                             const char* id) {
  pluginhttp::substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  for (const auto& kv : meta) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  char tsBuf[16];
  snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
  pluginhttp::substituteAll(tpl, "{event.ts}", tsBuf);
  pluginhttp::substituteAll(tpl, "{event.id}", id);
  for (JsonPairConst kv : vars.as<JsonObjectConst>()) {
    pluginhttp::substituteAll(tpl, (std::string("{event.") + kv.key().c_str() + "}").c_str(),
                              pluginhttp::variantToString(kv.value()));
  }
  return tpl;
}

// Replays one queued line. True = delivered (drop the line); false = transport
// or auth failure (keep it for the next drain). A line with no matching
// handler counts as delivered so a manifest edit can't wedge the queue.
// `token` is shared across the drain: a 401-minted refresh persists to the
// remaining lines instead of re-minting per line.
bool deliverLine(const DrainManifest& mf, const std::string& lineText, std::string& token,
                 const pluginhttp::Headers& config, GfxRenderer* renderer, uint32_t deadline = 0) {
  JsonDocument doc;
  if (deserializeJson(doc, lineText) != DeserializationError::Ok) return true;  // corrupt line: drop
  const int e = eventFromName(doc["e"] | "");
  const DrainManifest::Handler* handler = nullptr;
  for (const auto& h : mf.handlers) {
    if (h.event == e) {
      handler = &h;
      break;
    }
  }
  if (!handler) return true;

  JsonVariantConst vars = doc["vars"];
  const long long ts = doc["ts"] | 0LL;
  // Lines queued by pre-id firmware substitute {event.id} as empty.
  const char* id = doc["id"] | "";

  // Book-scoped events expose the book's plugin sidecar ("<book>.meta.json",
  // flat fields written at download time) as {meta.*} variables, e.g. a
  // service book id for a sync handler.
  pluginhttp::Headers meta;
  const char* book = vars["book"] | "";
  if (book[0] != '\0') {
    pluginhttp::loadConfigFile(std::string(book) + ".meta.json", meta);
    for (auto& kv : meta) kv.first = "{meta." + kv.first + "}";
  }

  const auto expired = [deadline]() { return deadline && static_cast<int32_t>(millis() - deadline) >= 0; };
  const auto timeout = [deadline]() -> uint32_t {
    if (!deadline) return 30000;
    const int32_t remaining = static_cast<int32_t>(deadline - millis());
    return remaining > 0 ? static_cast<uint32_t>(remaining) : 1;
  };
  const auto run = [&](const std::string& tok) {
    if (expired()) return -1;
    pluginhttp::Headers headers;
    headers.reserve(handler->req.headers.size());
    for (const auto& h : handler->req.headers) {
      headers.emplace_back(h.first, drainSubstituted(h.second, tok, config, meta, vars, ts, id));
    }
    if (handler->isDownload()) {
      const std::string dest = drainSubstituted(handler->dest, tok, config, meta, vars, ts, id);
      // Substituted fields must not climb out of the tree or land on a
      // credential store (same guard as the catalog sidecar writer).
      if (!protectedpaths::isPluginPath(dest)) {
        LOG_ERR("PEVT", "unsafe download dest rejected: %s", dest.c_str());
        return 200;  // treat as delivered: retrying can never fix the manifest
      }
      // Generous for images (a 4-bit 800x480 BMP is ~192KB), still bounded.
      constexpr size_t MAX_EVENT_DOWNLOAD = 1024 * 1024;
      // Stream to a sibling temp and swap it in only on a clean 2xx, so a
      // 404/500 error body can never replace an existing dest (e.g. /sleep.bmp).
      std::string tmp;
      if (!FilePublication::stagePath(dest, "plugin-event", tmp)) return -1;
      const int st = pluginhttp::requestToFile(
          nullptr, drainSubstituted(handler->req.url, tok, config, meta, vars, ts, id), handler->req.method,
          drainSubstituted(handler->req.body, tok, config, meta, vars, ts, id), headers, tmp.c_str(),
          MAX_EVENT_DOWNLOAD, expired, timeout());
      if (st >= 200 && st < 300) {
        if (!FilePublication::publish(tmp.c_str(), dest.c_str())) return -1;
        if (library::fileTypeFor(dest)) library::invalidateLibraryIndex();
      } else {
        Storage.remove(tmp.c_str());
      }
      return st;
    }
    String response;
    return pluginhttp::request(nullptr, drainSubstituted(handler->req.url, tok, config, meta, vars, ts, id),
                               handler->req.method,
                               drainSubstituted(handler->req.body, tok, config, meta, vars, ts, id), headers, response,
                               MAX_EVENT_RESPONSE, nullptr, expired, timeout());
  };

  int status = run(token);
  // A password-grant token expires; on 401/403 mint a fresh one and retry once.
  if ((status == 401 || status == 403) && mf.hasPasswordGrant()) {
    std::string minted;
    // Same template vocabulary as the delivery request (e.g. a {cfg.*} client
    // secret in an auth header).
    pluginhttp::Headers authHeaders;
    authHeaders.reserve(mf.authReq.headers.size());
    for (const auto& h : mf.authReq.headers) {
      authHeaders.emplace_back(h.first, drainSubstituted(h.second, token, config, meta, vars, ts, id));
    }
    if (pluginhttp::mintPasswordToken(nullptr, drainSubstituted(mf.authReq.url, token, config, meta, vars, ts, id),
                                      mf.authReq.method,
                                      drainSubstituted(mf.authReq.body, token, config, meta, vars, ts, id), authHeaders,
                                      mf.authTokenPath, minted, expired, timeout())) {
      pluginhttp::saveTokenToFile(mf.tokenFile, mf.tokenPath, minted);
      token = minted;
      status = run(token);
    }
  }
  if (status < 200 || status >= 300) return false;

  if (renderer && !handler->toast.empty()) {
    GUI.drawPopup(*renderer, drainSubstituted(handler->toast, token, config, meta, vars, ts, id).c_str());
  }
  return true;
}

}  // namespace

enum class DrainMode { Ordered, Sleep, Connect };
constexpr size_t MAX_SLEEP_CONNECT_DELIVERIES = 4;  // Match CrossPoint's default ordered drain.

static int queuedEvent(const std::string& raw, size_t start, size_t end) {
  JsonDocument event;
  if (deserializeJson(event, raw.data() + start, end - start) != DeserializationError::Ok) return -1;
  return eventFromName(event["e"] | "");
}
static void drainQueued(GfxRenderer* renderer, size_t maxEvents, DrainMode mode, uint32_t deadline = 0) {
  if (!SETTINGS.pluginsEnabled) return;
  std::lock_guard<std::mutex> guard(outboxMutex);
  size_t budget = maxEvents;
  const auto expired = [deadline]() { return deadline && static_cast<int32_t>(millis() - deadline) >= 0; };
  for (const auto& sub : subscribers) {
    if (budget == 0 || expired()) break;
    if (sub.name[0] == '\0') continue;
    const std::string path = outboxPath(sub);
    if (!DownloadFileSwap::recover(path) || !Storage.exists(path.c_str())) continue;

    std::string raw;
    if (!Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES + MAX_EVENT_LINE, raw)) continue;

    DrainManifest mf;
    // Unreadable or malformed right now (SD fault, a half-written manifest):
    // keep the queue for the next drain.
    if (!loadDrainManifest(sub, mf)) continue;
    if (mf.handlers.empty()) {
      // Subscribed but no runnable handlers (JS-only consumer, or manifest
      // edited away): the queue would never advance, so clear it.
      Storage.remove(path.c_str());
      continue;
    }
    std::string token;
    pluginhttp::Headers config;
    pluginhttp::loadTokenFromFile(mf.tokenFile, mf.tokenPath, token);
    pluginhttp::loadConfigFile(mf.configFile, config);
    // Pre-build the substitution patterns (see drainSubstituted).
    for (auto& kv : config) kv.first = "{cfg." + kv.first + "}";

    if (mode == DrainMode::Connect) {
      // Replay opt-in events in queue order. A failure stalls only this
      // subscriber. Keep all skipped, failed, and unattempted lines on disk.
      bool changed = false;
      for (size_t start = 0; start < raw.size() && budget > 0 && !expired();) {
        size_t nl = raw.find('\n', start);
        if (nl == std::string::npos) nl = raw.size();
        const size_t end = nl < raw.size() ? nl + 1 : nl;
        const int e = queuedEvent(raw, start, nl);
        if (e < 0 || e == static_cast<int>(Event::SleepEnter) || !(sub.connectMask & (1u << e))) {
          start = end;
          continue;
        }
        if (!deliverLine(mf, raw.substr(start, nl - start), token, config, renderer, deadline)) break;
        // The suffix slides into this offset; each source line is parsed once.
        raw.erase(start, end - start);
        changed = true;
        --budget;
      }
      if (changed) {
        if (raw.empty())
          Storage.remove(path.c_str());
        else
          writeOutbox(path, raw.data(), raw.size());
      }
      continue;
    }

    if (mode == DrainMode::Sleep) {
      // Prioritize the latest sleep event for this plugin. Preserve all other
      // queued events, including old failed sleep events, for ordinary replay.
      size_t selected = std::string::npos, end = 0;
      for (size_t start = 0; start < raw.size();) {
        size_t next = raw.find('\n', start);
        if (next == std::string::npos) next = raw.size();
        if (queuedEvent(raw, start, next) == static_cast<int>(Event::SleepEnter)) {
          selected = start;
          end = next < raw.size() ? next + 1 : next;
        }
        start = next + 1;
      }
      if (selected != std::string::npos) {
        --budget;
        if (deliverLine(mf, raw.substr(selected, end - selected), token, config, renderer, deadline)) {
          raw.erase(selected, end - selected);
          if (raw.empty())
            Storage.remove(path.c_str());
          else
            writeOutbox(path, raw.data(), raw.size());
        }
      }
      continue;
    }

    // Walk lines; stop at the first failure so order is preserved.
    size_t pos = 0;
    bool stalled = false;
    while (pos < raw.size() && budget > 0 && !stalled) {
      size_t nl = raw.find('\n', pos);
      if (nl == std::string::npos) nl = raw.size();
      const std::string lineText = raw.substr(pos, nl - pos);
      if (!lineText.empty()) {
        --budget;
        if (!deliverLine(mf, lineText, token, config, renderer)) {
          stalled = true;
          break;
        }
      }
      pos = nl + 1;
    }

    if (pos >= raw.size() && !stalled) {
      Storage.remove(path.c_str());
    } else if (pos > 0) {
      // Publish the tail without risking undelivered events on a short write.
      writeOutbox(path, raw.data() + pos, raw.size() - pos);
    }
    LOG_DBG("PEVT", "%s: drained (stalled=%d)", sub.name, stalled);
  }
}

void drain(GfxRenderer* renderer, size_t maxEvents) { drainQueued(renderer, maxEvents, DrainMode::Ordered); }

void pollOnline() {
  static uint32_t last = 0;
  static int lastEnabled = -1;
  if (lastEnabled != SETTINGS.pluginsEnabled) {
    lastEnabled = SETTINGS.pluginsEnabled;
    refreshSubscriptions();
  }
  if (!SETTINGS.pluginsEnabled || WiFi.status() != WL_CONNECTED || millis() - last < 30000) return;
  last = millis();
  trustedtime::startSync();
  refreshSubscriptions();
  drain(nullptr, 1);
}
void queueSleep(const char* book, int progress) {
  if (!SETTINGS.pluginsEnabled) return;
  refreshSubscriptions();
  char percent[8];
  snprintf(percent, sizeof(percent), "%d", progress);
  const Var vars[] = {{"book", book}, {"percent", percent}};
  emit(Event::SleepEnter, vars, book[0] ? 2 : 0);
}

bool deliverSleepEvents() {
  if (!SETTINGS.pluginsEnabled) return false;
  bool attemptedJoin = false;
  if (WiFi.status() != WL_CONNECTED && wantsConnectAny()) {
    const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (credential) {
      attemptedJoin = true;
      WiFi.mode(WIFI_STA);
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
      const uint32_t start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(50);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    const uint32_t deadline = millis() + 10000;
    drainQueued(nullptr, MAX_EVENT_PLUGINS, DrainMode::Sleep, deadline);
    drainQueued(nullptr, MAX_SLEEP_CONNECT_DELIVERIES, DrainMode::Connect, deadline);
  }
  return attemptedJoin;
}

}  // namespace pluginevents
