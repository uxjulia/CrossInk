#include <ArduinoJson.h>
#include <BookKey.h>
#include <DeviceSecret.h>
#include <Util.h>
#include <WolfsslCrypto.h>
#include <base64.h>
#include <wolfssl/wolfcrypt/aes.h>

#include "CrossPointSettings.h"
#include "CrossPointWebServer.h"
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryFileTypes.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Memory.h>
#include <ResumableFetch.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "DownloadFileSwap.h"
#include "ProtectedPaths.h"
#include "SdCardFontSystem.h"
#include "WifiPowerSaveGuard.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "html/RunnerPageHtml.generated.h"
#include "util/BookCacheUtils.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/TaskWatchdog.h"
void CrossPointWebServer::streamFileToClient(HalFile& file) const {
  NetworkClient client = server->client();
  static constexpr size_t CHUNK_SIZE = 4096;
  // Off the stack: the web-server task also runs TLS and SD from this stack.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK_SIZE);
  if (!buffer) {
    LOG_ERR("WEB", "OOM: %u byte stream buffer", (unsigned)CHUNK_SIZE);
    return;
  }

  bool ok = true;
  while (ok && file.available()) {
    const int result = file.read(buffer.get(), CHUNK_SIZE);
    if (result <= 0) break;
    const size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      resetTaskWatchdogIfSubscribed();
      const size_t wrote = client.write(buffer.get() + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        ok = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
}
namespace {

// A path component is safe if it has no separators or parent refs.
bool safeComponent(const String& s) { return s.length() == strlen(s.c_str()) && PluginLocations::validName(s.c_str()); }

const char* pluginContentType(const String& file) {
  if (file.endsWith(".js")) return "application/javascript";
  if (file.endsWith(".css")) return "text/css";
  if (file.endsWith(".html")) return "text/html";
  if (file.endsWith(".json")) return "application/json";
  if (file.endsWith(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

}  // namespace

bool CrossPointWebServer::readJsonBody(JsonDocument& out) const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"missing body\"}");
    return false;
  }
  if (server->arg("plain").length() > 16 * 1024) {
    server->send(413, "application/json", "{\"error\":\"request too large\"}");
    return false;
  }
  if (deserializeJson(out, server->arg("plain")) != DeserializationError::Ok) {
    server->send(400, "application/json", "{\"error\":\"bad json\"}");
    return false;
  }
  return true;
}

void CrossPointWebServer::sendJson(const JsonDocument& doc) const {
  String out;
  if (!out.reserve(measureJson(doc))) {
    LOG_ERR("WEB", "OOM: JSON response");
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  serializeJson(doc, out);
  server->send(200, "application/json", out);
}

// GET /api/plugins -> [{ "name", "title", "mount" }, ...]. Only plugins with a
// plugin.js are listed (the page loads it); optional manifest.json supplies the
// title and mount point.
bool CrossPointWebServer::rejectIfPluginsDisabled() const {
  if (SETTINGS.pluginsEnabled) return false;
  server->send(403, "text/plain", "plugin system disabled");
  return true;
}

void CrossPointWebServer::handlePluginList() const {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : PluginLocations::scanPlugins()) {
    if (!e.hasPluginJs) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = e.name;
    obj["dir"] = e.dir;         // the plugin keeps its own files here
    obj["title"] = e.name;      // overridden by manifest below
    obj["mount"] = "settings";  // default mount point
    std::string manifest;
    if (e.hasManifest && Storage.readFileToString("WEB", e.dir + "/manifest.json", 8 * 1024, manifest)) {
      JsonDocument m;
      if (deserializeJson(m, manifest) == DeserializationError::Ok) {
        if (m["title"].is<const char*>()) obj["title"] = m["title"];
        if (m["mount"].is<const char*>()) obj["mount"] = m["mount"];
      }
    }
  }

  sendJson(doc);
}

// GET /plugin?name=<plugin>&file=<file> -> serve /.crosspoint/plugins/<plugin>/<file>
void CrossPointWebServer::handlePluginFile() const {
  if (rejectIfPluginsDisabled()) return;
  const String name = server->arg("name");
  const String file = server->arg("file");
  if (!safeComponent(name) || !safeComponent(file)) {
    server->send(400, "text/plain", "bad plugin path");
    return;
  }
  const std::string pluginDir = PluginLocations::findPluginDir(name.c_str());
  if (pluginDir.empty()) {
    server->send(404, "text/plain", "not found");
    return;
  }
  const std::string path = pluginDir + "/" + file.c_str();
  HalFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f || !f.isOpen() || f.isDirectory()) {
    f.close();
    server->send(404, "text/plain", "not found");
    return;
  }

  // Hash served bytes: timestamps and file sizes can stay unchanged after an update.
  MD5Builder md5;
  md5.begin();
  uint8_t chunk[128];
  const size_t expected = f.size();
  size_t hashed = 0;
  int got;
  while ((got = f.read(chunk, sizeof(chunk))) > 0) {
    md5.add(chunk, static_cast<size_t>(got));
    hashed += static_cast<size_t>(got);
    resetTaskWatchdogIfSubscribed();
  }
  if (got < 0 || hashed != expected || !f.seek(0)) {
    f.close();
    LOG_ERR("WEB", "Plugin file read failed");
    server->send(500, "text/plain", "Failed to read file");
    return;
  }
  md5.calculate();
  const String etag = "\"" + md5.toString() + "\"";
  server->sendHeader("ETag", etag);
  server->sendHeader("Cache-Control", "no-cache");
  if (server->header("If-None-Match") == etag) {
    f.close();
    server->setContentLength(0);
    server->send(304);
    return;
  }
  server->setContentLength(f.size());
  server->send(200, pluginContentType(file), "");
  streamFileToClient(f);
  f.close();
}

// POST /api/relay {plugin, method, url, headers:{}, body}
//   -> 200 with the upstream body raw, its status in X-Relay-Status and its
//      headers in X-Relay-Headers ([[name, value], ...] JSON, duplicates kept)
// Lets a plugin make an outbound HTTP(S) call the browser can't (CORS): the
// device makes it via SecureNet. Sending the body raw avoids escaping it into
// JSON on the device; PluginHost.relay() rebuilds {status, headers, body}.
void CrossPointWebServer::handleRelay() {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const String plugin = req["plugin"] | "";
  const std::string url = req["url"] | "";
  const std::string method = req["method"] | "GET";
  if (!safeComponent(plugin) || url.empty()) {
    server->send(400, "application/json", "{\"error\":\"missing plugin/url\"}");
    return;
  }
  pluginhttp::Headers headers;
  pluginhttp::readHeaders(req["headers"], headers);
  const std::string body = req["body"] | "";
  // All values needed below now have independent storage. Drop both copies of
  // the inbound JSON before wolfSSL allocates its handshake working set.
  req.clear();
  req.shrinkToFit();
  releasePluginRequest();

  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
  LOG_DBG("WEB", "Relay TLS start: heap %u, max block %u: %s", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap(), url.c_str());

  // One bounded body buffer; larger payloads use /api/fetch. This task is
  // subscribed to the task WDT for the whole web-server session, so feed it
  // while a slow peer keeps the request waiting.
  static constexpr size_t RELAY_BODY_LIMIT = 32 * 1024;
  String respBody;
  pluginhttp::Headers respHeaders;
  const int status =
      pluginhttp::request(nullptr, url, method, body, headers, respBody, RELAY_BODY_LIMIT, &respHeaders, [] {
        resetTaskWatchdogIfSubscribed();
        return false;  // timeout is enforced by the shared transport
      });
  // Transport failure, a truncated body, or one over the cap / out of memory
  // (the reason is logged by pluginhttp).
  if (status < 0) {
    server->send(502, "application/json", "{\"error\":\"relay failed; large bodies need /api/fetch\"}");
    return;
  }

  JsonDocument headersDoc;
  JsonArray headerArray = headersDoc.to<JsonArray>();
  for (const auto& h : respHeaders) {
    JsonArray pair = headerArray.add<JsonArray>();
    pair.add(h.first);
    pair.add(h.second);
  }
  String headersJson;
  serializeJson(headersDoc, headersJson);
  server->sendHeader("X-Relay-Status", String(status));
  server->sendHeader("X-Relay-Headers", headersJson);
  server->send(200, "application/octet-stream", respBody);
}

// Reserved upstream APIs: fail explicitly until protected-book integration ships.
void CrossPointWebServer::handleCrypto() {
  if (rejectIfPluginsDisabled()) return;
  using namespace freeink::content;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string_view op = req["op"] | "";
  const auto sendOom = [this](const char* operation) {
    LOG_ERR("WEB", "OOM: crypto %s", operation);
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
  };
  struct Bytes {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
    // Never null, so an empty field is still a valid zero-length input.
    const uint8_t* ptr() const {
      static constexpr uint8_t EMPTY = 0;
      return data ? data.get() : &EMPTY;
    }
  };
  // Nothrow decode of a base64 field: a bundle-sized input under heap pressure
  // must fail the op, not abort() the device (-fno-exceptions). The cap keeps
  // a LAN client from posting a multi-megabyte value; crypto inputs (keys,
  // certs, PKCS#12 bundles) are a few KB.
  bool decodeOom = false;
  auto dec = [&](const char* field) {
    static constexpr size_t MAX_CRYPTO_FIELD = 64 * 1024;
    Bytes out;
    const char* v = req[field].as<const char*>();
    const size_t encodedLen = v ? strlen(v) : 0;
    if (encodedLen == 0 || encodedLen > MAX_CRYPTO_FIELD) return out;
    const size_t cap = encodedLen * 3 / 4 + 3;
    out.data = makeUniqueNoThrow<uint8_t[]>(cap);
    if (!out.data) decodeOom = true;
    const int32_t n = out.data ? base64Decode(v, encodedLen, out.data.get(), cap) : -1;
    out.size = n < 0 ? 0 : static_cast<size_t>(n);
    return out;
  };

  JsonDocument resp;
  bool encodeFailed = false;
  const auto setEncoded = [&](const char* name, const uint8_t* data, size_t size) {
    const uint8_t empty = 0;
    if (size == 0) data = &empty;
    const String encoded = base64::encode(data, size);
    if ((size && encoded.isEmpty()) || encoded == "-FAIL-") {
      encodeFailed = true;
      return;
    }
    resp[name] = encoded;
  };
  const auto setEncodedVector = [&](const char* name, const std::vector<uint8_t>& data) {
    setEncoded(name, data.data(), data.size());
  };

  WolfsslCrypto c;

  if (op == "random") {
    static constexpr int MAX_RANDOM_BYTES = 4096;  // generous for keys/salts/tokens; blocks a runaway allocation
    const int n = std::clamp(static_cast<int>(req["len"] | 16), 0, MAX_RANDOM_BYTES);
    auto out = makeUniqueNoThrow<uint8_t[]>(n);
    if (n && !out) {
      sendOom("random output");
      return;
    }
    if (n && !secureRandomBytes(out.get(), n)) {
      server->send(503, "application/json", "{\"error\":\"random unavailable\"}");
      return;
    }
    const uint8_t empty = 0;
    setEncoded("data", n ? out.get() : &empty, n);
  } else if (op == "sha1") {
    const Bytes d = dec("data");
    if (decodeOom) return sendOom("input");
    uint8_t h[20];
    c.sha1(d.ptr(), d.size, h);
    setEncoded("data", h, 20);
  } else if (op == "aesenc" || op == "aesdec") {
    const Bytes k = dec("key"), iv = dec("iv"), d = dec("data");
    if (decodeOom) return sendOom("input");
    if (k.size != 16 || iv.size != 16) {
      resp["error"] = "key/iv must be 16 bytes";
    } else if (op == "aesenc") {
      const size_t outSize = ((d.size / 16) + 1) * 16;
      auto out = makeUniqueNoThrow<uint8_t[]>(outSize);
      // Pad inside the output buffer; the SDK helper allocates a second copy.
      auto aes = makeUniqueNoThrow<Aes>();
      if (!out || !aes) return sendOom("AES output");
      memcpy(out.get(), d.ptr(), d.size);
      memset(out.get() + d.size, static_cast<int>(outSize - d.size), outSize - d.size);
      const bool initialized = wc_AesInit(aes.get(), nullptr, INVALID_DEVID) == 0;
      if (initialized && wc_AesSetKey(aes.get(), k.ptr(), 16, iv.ptr(), AES_ENCRYPTION) == 0 &&
          wc_AesCbcEncrypt(aes.get(), out.get(), out.get(), outSize) == 0)
        setEncoded("data", out.get(), outSize);
      else
        resp["error"] = "aesenc failed";
      if (initialized) wc_AesFree(aes.get());
    } else if (d.size % 16 != 0) {
      resp["error"] = "data not block-aligned";
    } else {
      auto out = makeUniqueNoThrow<uint8_t[]>(d.size);
      if (d.size && !out) {
        return sendOom("AES output");
      }
      uint8_t empty = 0;
      if (c.aes128CbcDecrypt(k.ptr(), iv.ptr(), d.ptr(), d.size, d.size ? out.get() : &empty))
        setEncoded("data", d.size ? out.get() : &empty, d.size);
      else
        resp["error"] = "aesdec failed";
    }
  } else if (op == "sha256") {
    const Bytes d = dec("data");
    if (decodeOom) return sendOom("input");
    uint8_t h[32];
    c.sha256(d.ptr(), d.size, h);
    setEncoded("data", h, 32);
  } else if (op == "rsadec") {
    // Raw private-key operation with a caller-supplied PKCS#8 key; the caller
    // removes the padding.
    const Bytes priv = dec("private"), d = dec("data");
    if (decodeOom) return sendOom("input");
    static constexpr size_t MAX_MODULUS = 512;
    auto out = makeUniqueNoThrow<uint8_t[]>(MAX_MODULUS);
    if (!out) return sendOom("RSA output");
    const int32_t n = c.rsaPrivateRaw(priv.ptr(), priv.size, d.ptr(), d.size, out.get(), MAX_MODULUS);
    if (n > 0)
      setEncoded("data", out.get(), static_cast<size_t>(n));
    else
      resp["error"] = "rsadec failed";
  } else if (op == "keygen") {
    RsaKeyPairDer kp;
    if (c.rsaGenerate(&kp)) {
      setEncodedVector("public", kp.spki);
      setEncodedVector("private", kp.pkcs8);
    } else {
      resp["error"] = "keygen failed: " + c.lastError;
    }
  } else if (op == "pubencrypt") {
    const Bytes cert = dec("cert"), d = dec("data");
    if (decodeOom) return sendOom("input");
    auto out = makeUniqueNoThrow<uint8_t[]>(512);  // off the stack; RSA output up to 4096-bit
    if (!out) {
      return sendOom("RSA output");
    }
    size_t olen = 0;
    if (c.rsaPublicEncrypt(cert.ptr(), cert.size, d.ptr(), d.size, out.get(), 512, &olen))
      setEncoded("data", out.get(), olen);
    else
      resp["error"] = "pubencrypt failed: " + c.lastError + " (cert " + std::to_string(cert.size) + "B, data " +
                      std::to_string(d.size) + "B)";
  } else if (op == "sign") {
    const Bytes priv = dec("private"), h = dec("hash");
    if (decodeOom) return sendOom("input");
    uint8_t sig[128];
    if (h.size != 20)
      resp["error"] = "hash must be 20 bytes";
    else if (c.rsaPrivateSignRaw(priv.ptr(), priv.size, h.ptr(), sig))
      setEncoded("data", sig, 128);
    else
      resp["error"] = "sign failed";
  } else if (op == "pkcs12") {
    const Bytes p12 = dec("data");
    if (decodeOom) return sendOom("input");
    const std::string pw = req["password"] | "";
    // The decoded bundle no longer depends on the request document. Reclaim
    // its large base64 string before the KDF and certificate parsing begin.
    req.clear();
    std::vector<uint8_t> key, cert;
    if (p12.size == 0) {
      resp["error"] = "pkcs12 failed: bundle missing, invalid, or out of memory";
    } else if (c.pkcs12Extract(p12.ptr(), p12.size, pw, &key, &cert)) {
      setEncodedVector("key", key);
      setEncodedVector("cert", cert);
    } else {
      resp["error"] = "pkcs12 failed: " + c.lastError;
    }
  } else {
    resp["error"] = "unknown op";
  }

  if (encodeFailed || resp.overflowed()) {
    return sendOom("response");
  }
  sendJson(resp);
}

// POST /api/fetch {plugin, url, dest, headers?, offset?, maxBytes?}
//   -> {status, bytes, complete, total?}
// Device downloads a URL straight to SD, so a large body never passes through
// the browser.
void CrossPointWebServer::handleFetch() {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string url = req["url"] | "";
  const std::string dest = req["dest"] | "";
  const size_t requestedOffset = req["offset"] | 0;
  size_t segmentLimit = req["maxBytes"] | 0;
  static constexpr size_t FETCH_MAX_SEGMENT_SIZE = 4 * 1024 * 1024;
  if (segmentLimit > FETCH_MAX_SEGMENT_SIZE) segmentLimit = FETCH_MAX_SEGMENT_SIZE;
  if (url.empty() || !protectedpaths::isPluginPath(dest)) {
    server->send(400, "application/json", "{\"error\":\"bad url/dest\"}");
    return;
  }

  pluginhttp::Headers requestHeaders;
  pluginhttp::readHeaders(req["headers"], requestHeaders);
  if (!pluginhttp::validRequest(url, "GET") || !pluginhttp::validHeaders(requestHeaders)) {
    server->send(400, "application/json", "{\"error\":\"invalid request\"}");
    return;
  }
  req.clear();
  req.shrinkToFit();
  releasePluginRequest();

  if (!DownloadFileSwap::recover(dest)) {
    server->send(500, "application/json", "{\"error\":\"cannot recover previous file\"}");
    return;
  }
  // Stage in <dest>.part so an interrupted or abandoned download never sits
  // under the real name, and an existing dest survives until the new copy is complete.
  const std::string part = dest + ".part";
  HalFile file;
  if (requestedOffset == 0) {
    // Mirror handlePluginFs(): create missing parents so a plugin's first fetch
    // into a fresh subfolder (e.g. /.crosspoint/plugins/<name>/) doesn't fail
    // before anything has a chance to create it.
    const size_t lastSlash = dest.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      Storage.ensureDirectoryExists(dest.substr(0, lastSlash).c_str());
    }
    Storage.remove(part.c_str());
    if (!Storage.openFileForWrite("PLG", part, file)) {
      server->send(500, "application/json", "{\"error\":\"cannot create file\"}");
      return;
    }
  } else {
    file = Storage.open(part.c_str(), O_RDWR | O_AT_END);
    const size_t existingSize = file ? file.size() : 0;
    if (!file || existingSize != requestedOffset) {
      if (file) file.close();
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"error\":\"offset mismatch\",\"bytes\":%u}", (unsigned)existingSize);
      server->send(409, "application/json", msg);
      return;
    }
  }

  // Resume and Range-restart handling live in fetchResumable (ResumableFetch.h).
  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
  WifiPowerSaveGuard psGuard;

  size_t written = requestedOffset;
  size_t nextHeapLog = written;
  bool sdFull = false;
  bool segmentBoundary = false;
  bool rangeUnsupported = false;
  const unsigned long fetchStartedAt = millis();
  unsigned long lastBrowserHeartbeat = fetchStartedAt;
  bool browserResponseStarted = false;

  // A phone may discard an HTTP response that sends no bytes for several
  // minutes even while the device is actively downloading upstream. Start a
  // chunked JSON response only once the operation becomes long-running, then
  // send JSON whitespace to keep that browser-facing connection active.
  const auto keepBrowserAlive = [this, &browserResponseStarted, &lastBrowserHeartbeat]() {
    const unsigned long now = millis();
    if (now - lastBrowserHeartbeat < 5000) return;
    lastBrowserHeartbeat = now;
    if (!server->client().connected()) return;
    if (!browserResponseStarted) {
      server->setContentLength(CONTENT_LENGTH_UNKNOWN);
      server->send(200, "application/json", "");
      browserResponseStarted = true;
    }
    server->sendContent(" \n", 2);
  };
  const auto sendFetchResult = [this, &browserResponseStarted](int code, const String& payload) {
    if (!browserResponseStarted) {
      server->send(code, "application/json", payload);
      return;
    }
    if (server->client().connected()) {
      server->sendContent(payload);
      server->sendContent("", 0);
    }
  };

  freeink::FetchOptions options;
  options.startOffset = requestedOffset;
  freeink::FetchSink sink;
  sink.write = [&](const uint8_t* data, size_t len) {
    resetTaskWatchdogIfSubscribed();
    const size_t writeLen = segmentLimit > 0 ? std::min(len, requestedOffset + segmentLimit - written) : len;
    if (file.write(data, writeLen) != writeLen) {
      sdFull = true;
      return false;
    }
    written += writeLen;
    // Heap trajectory during the transfer: a steady value rules RAM out of a
    // mid-body failure; a falling one implicates it.
    if (written >= nextHeapLog) {
      LOG_DBG("WEB", "Fetch %u bytes, heap %u", (unsigned)written, (unsigned)ESP.getFreeHeap());
      nextHeapLog = written + 1024 * 1024;
    }
    keepBrowserAlive();
    // A bounded segment stops here; the next browser request resumes from
    // `written` with Range.
    if (segmentLimit > 0 && written - requestedOffset >= segmentLimit) {
      segmentBoundary = true;
      return false;
    }
    return true;
  };
  sink.rewind = [&] {
    // Range ignored: the body restarts from byte 0, which only a transfer
    // that has not yet reported progress to the browser can follow.
    if (requestedOffset > 0) {
      rangeUnsupported = true;
      return false;
    }
    file.close();
    written = 0;
    return Storage.openFileForWrite("PLG", part, file);
  };
  const auto shouldAbort = [&] {
    resetTaskWatchdogIfSubscribed();
    keepBrowserAlive();
    return static_cast<uint32_t>(millis() - fetchStartedAt) >= 300000 || dropUploadIfCancelled();
  };
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        http.setUserAgent("CrossPoint");
        // The SecureNet transport ships no CA bundle, so peer verification always
        // fails (wolfSSL -188); skip it like HttpDownloader does. Traffic stays
        // TLS-encrypted, just unauthenticated — matching the prior library-lending flow.
        http.setInsecure();
        // Some delivery servers assemble books on the fly and can stall mid-body
        // while packaging; the default 15s no-data timeout truncates those downloads.
        http.setTimeout(60000);
        // The plugin's headers (typically its Authorization) stay with the
        // starting origin; a redirect to another server gets none of them.
        if (sameOrigin && !requestHeaders.empty()) {
          // Do not retain the client's header copies during the TLS handshake.
          http.connect(shouldAbort);
          for (const auto& header : requestHeaders) http.addHeader(header.first, header.second);
        }
      },
      sink, shouldAbort);
  if (file.isOpen()) {
    if (!file.close()) sdFull = true;
  }
  const int status = result.status;
  const size_t totalExpected = result.total;
  bool complete = !sdFull && (result.complete || (segmentBoundary && totalExpected > 0 && written >= totalExpected));

  const bool ok2xx = status >= 200 && status < 300;
  // A bounded segment ended mid-body: the browser requests the next one, so
  // the .part stays and nothing is installed yet.
  const bool midSegment = segmentBoundary && !sdFull && !complete && ok2xx;

  if (!complete && ok2xx && !midSegment) {
    Storage.remove(part.c_str());
    char msg[96];
    const char* error = sdFull ? "sd write failed" : rangeUnsupported ? "range unsupported" : "download truncated";
    // complete:false matters once the heartbeat has committed HTTP 200 chunked:
    // it is the only signal fetchToSd()'s resume loop still sees on this path
    // (it then detects zero progress and throws instead of returning success).
    snprintf(msg, sizeof(msg), "{\"error\":\"%s\",\"bytes\":%u,\"complete\":false}", error, (unsigned)written);
    LOG_ERR("WEB", "Fetch failed after %u bytes in %lu ms: %s", (unsigned)written, millis() - fetchStartedAt,
            url.c_str());
    sendFetchResult(502, msg);
    return;
  }

  JsonDocument resp;
  if (!ok2xx) {
    Storage.remove(part.c_str());
    resp["error"] = status < 0 ? "transport failure" : "http status";
  } else if (complete && !DownloadFileSwap::publish(dest)) {
    Storage.remove(part.c_str());
    complete = false;
    resp["error"] = "sd write failed";
  }
  if (ok2xx && complete) {
    clearBookCache(dest);
    if (library::fileTypeFor(dest)) {
      library::invalidateLibraryIndex();
      const pluginevents::Var vars[] = {{"path", dest.c_str()}, {"title", ""}, {"plugin", req["plugin"] | ""}};
      pluginevents::emit(pluginevents::Event::BookDownloaded, vars, 3);
    }
    sdFontSystem.markRegistryDirtyForPath(dest.c_str());
    ImageFolderIndex::invalidateForPath(dest.c_str());
  }
  resp["status"] = status;
  resp["bytes"] = written;
  resp["complete"] = complete;
  if (totalExpected > 0) resp["total"] = totalExpected;
  String out;
  serializeJson(resp, out);
  const bool browserConnected = server->client().connected();
  LOG_INF("WEB", "Fetch %s: %u bytes in %lu ms, browser %s: %s",
          midSegment ? "segment done"
          : complete ? "complete"
                     : "failed",
          (unsigned)written, millis() - fetchStartedAt, browserConnected ? "connected" : "disconnected", url.c_str());
  sendFetchResult(200, out);
}

// POST /api/plugin-fs?plugin=<name>&path=<path> with the file contents as a
// multipart file part. A plugin writes a small file to SD. Multipart, not a raw
// body: WebServer turns a plain body into a NUL-terminated String (truncating
// binary data) and buffers all of it first, while file parts stream in chunks.
void CrossPointWebServer::handlePluginFsUpload() {
  static constexpr size_t MAX_PLUGIN_FILE = 256 * 1024;
  auto& st = pluginFsUpload;
  const HTTPUpload& part = server->upload();
  const auto fail = [&st](const int status, const char* error) {
    if (st.file.isOpen()) st.file.close();  // explicit: remove follows on the same path
    if (!st.tmp.empty()) Storage.remove(st.tmp.c_str());
    st.errorStatus = status;
    st.error = error;
  };

  switch (part.status) {
    case UPLOAD_FILE_START: {
      if (st.file.isOpen()) st.file.close();
      if (!st.tmp.empty()) Storage.remove(st.tmp.c_str());
      st.path = server->arg("path").c_str();
      st.tmp.clear();
      st.bytes = 0;
      st.started = true;
      st.errorStatus = 0;
      st.error = nullptr;
      if (!SETTINGS.pluginsEnabled) {
        fail(403, "plugin system disabled");
        return;
      }
      const String plugin = server->arg("plugin");
      if (!safeComponent(plugin) || !protectedpaths::isPluginPath(st.path)) {
        LOG_ERR("WEB", "Rejected plugin file write: plugin='%s' path='%s'", plugin.c_str(), st.path.c_str());
        fail(400, "bad path");
        return;
      }
      // ensureDirectoryExists() creates missing parents along the way, so this
      // covers any depth under /.crosspoint/plugins/<name>/... in one call.
      const size_t lastSlash = st.path.rfind('/');
      if (lastSlash != std::string::npos && lastSlash > 0) {
        Storage.ensureDirectoryExists(st.path.substr(0, lastSlash).c_str());
      }
      st.tmp = st.path + ".tmp";
      Storage.remove(st.tmp.c_str());
      if (!Storage.openFileForWrite("PLG", st.tmp, st.file)) fail(500, "cannot write");
      return;
    }
    case UPLOAD_FILE_WRITE:
      if (st.errorStatus) return;
      if (!st.started || !st.file.isOpen() || dropUploadIfCancelled()) {
        fail(400, "upload aborted");
        return;
      }
      if (part.currentSize > MAX_PLUGIN_FILE - st.bytes) {
        fail(413, "too large");
        return;
      }
      resetTaskWatchdogIfSubscribed();
      if (st.file.write(part.buf, part.currentSize) != part.currentSize) {
        fail(500, "sd write failed");
        return;
      }
      st.bytes += part.currentSize;
      return;
    case UPLOAD_FILE_END:
      if (st.errorStatus) return;
      if (!st.started || dropUploadIfCancelled()) {
        fail(400, "upload aborted");
        return;
      }
      if (!st.file.close()) {
        fail(500, "sd write failed");
        return;
      }
      // An empty body must not replace existing credentials with nothing.
      if (st.bytes == 0) {
        fail(400, "empty body");
      } else if (!DownloadFileSwap::publishFrom(st.path, st.tmp)) {
        fail(500, "sd write failed");
      } else {
        clearBookCache(st.path);
        if (library::fileTypeFor(st.path)) {
          library::invalidateLibraryIndex();
          const std::string plugin = server->arg("plugin").c_str();
          const pluginevents::Var vars[] = {{"path", st.path.c_str()}, {"title", ""}, {"plugin", plugin.c_str()}};
          pluginevents::emit(pluginevents::Event::BookDownloaded, vars, 3);
        }
        sdFontSystem.markRegistryDirtyForPath(st.path.c_str());
        ImageFolderIndex::invalidateForPath(st.path.c_str());
      }
      return;
    case UPLOAD_FILE_ABORTED:
      fail(400, "upload aborted");
      return;
  }
}

void CrossPointWebServer::handlePluginFs() {
  auto& st = pluginFsUpload;
  if (!st.started) {
    server->send(400, "application/json", "{\"error\":\"missing file part\"}");
  } else if (st.errorStatus) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"error\":\"%s\"}", st.error);
    server->send(st.errorStatus, "application/json", msg);
  } else {
    JsonDocument resp;
    resp["ok"] = true;
    resp["bytes"] = st.bytes;
    sendJson(resp);
  }
  st.started = false;
}

// POST /api/book-key {path, key (base64, 16 bytes), expires?} -> {ok}
// Reserved for a later protected-book integration.
void CrossPointWebServer::handleBookKey() {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string path = req["path"] | "";
  const char* keyB64 = req["key"] | "";
  const int64_t expires = req["expires"] | static_cast<int64_t>(0);
  uint8_t key[bookkey::MAX_KEY_LEN];
  const int32_t n = freeink::content::base64Decode(keyB64, strlen(keyB64), key, sizeof(key));
  if (!protectedpaths::isPluginPath(path) || !FsHelpers::hasEpubExtension(std::string_view(path)) ||
      (n != 16 && n != 32) || expires < 0 || (!req["expires"].isNull() && !req["expires"].is<int64_t>())) {
    server->send(400, "application/json", "{\"error\":\"bad path/key\"}");
    return;
  }
  if (!bookkey::write(path, key, static_cast<size_t>(n), expires)) {
    server->send(500, "application/json", "{\"error\":\"cannot store key\"}");
    return;
  }
  server->send(200, "application/json", "{\"ok\":true}");
}

void CrossPointWebServer::handlePluginRunnerPage() const {
  if (rejectIfPluginsDisabled()) return;
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", RunnerPageHtml, RunnerPageHtmlCompressedSize);
  LOG_DBG("WEB", "Served plugin runner page");
}

CrossPointWebServer::PluginJob* CrossPointWebServer::allocPluginJob() {
  PluginJob* best = nullptr;
  for (auto& job : pluginJobs) {
    if (job.state == JOB_EMPTY) return &job;
    const bool finished = job.state == JOB_DONE || job.state == JOB_ERROR;
    if (finished && (!best || job.updatedAt < best->updatedAt)) best = &job;
  }
  return best;
}

// POST /api/plugin-jobs {plugin, action, args?} -> {id}
void CrossPointWebServer::handlePluginJobSubmit() {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const String plugin = req["plugin"] | "";
  const String action = req["action"] | "";
  // The claim response embeds action without JSON escaping.
  const auto identifierSafe = [](const String& s) {
    for (const char c : s) {
      if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return !s.isEmpty();
  };
  if (!safeComponent(plugin) || !identifierSafe(action) || plugin.length() >= sizeof(PluginJob::plugin) ||
      action.length() >= sizeof(PluginJob::action) ||
      (!req["args"].isNull() && measureJson(req["args"]) >= sizeof(PluginJob::args))) {
    server->send(400, "application/json", "{\"error\":\"bad plugin/action/args\"}");
    return;
  }
  PluginJob* job = allocPluginJob();
  if (!job) {
    server->send(503, "application/json", "{\"error\":\"job queue full\"}");
    return;
  }
  *job = PluginJob{};
  job->id = nextPluginJobId++;
  job->state = JOB_PENDING;
  job->updatedAt = millis();
  snprintf(job->plugin, sizeof(job->plugin), "%s", plugin.c_str());
  snprintf(job->action, sizeof(job->action), "%s", action.c_str());
  if (!req["args"].isNull()) serializeJson(req["args"], job->args, sizeof(job->args));
  LOG_INF("WEB", "Plugin job %u queued: %s/%s", (unsigned)job->id, job->plugin, job->action);
  char msg[48];
  snprintf(msg, sizeof(msg), "{\"id\":%u}", (unsigned)job->id);
  server->send(200, "application/json", msg);
}

// GET /api/plugin-jobs/claim?plugin=<name> -> {id, action, args} or {id:0}
void CrossPointWebServer::handlePluginJobClaim() {
  if (rejectIfPluginsDisabled()) return;
  const String plugin = server->arg("plugin");
  const uint32_t now = millis();
  for (auto& job : pluginJobs) {
    if (job.state == JOB_RUNNING && now - job.updatedAt > PLUGIN_JOB_LEASE_MS) {
      job.state = JOB_PENDING;
      job.updatedAt = now;
      LOG_INF("WEB", "Plugin job %u lease expired; requeued", (unsigned)job.id);
    }
    if (job.state != JOB_PENDING || plugin != job.plugin) continue;
    job.state = JOB_RUNNING;
    job.claim = nextPluginJobClaim++;
    job.updatedAt = now;
    const std::string msg = "{\"id\":" + std::to_string(job.id) + ",\"claim\":" + std::to_string(job.claim) +
                            ",\"action\":\"" + job.action + "\",\"args\":" + (job.args[0] ? job.args : "{}") + "}";
    server->send(200, "application/json", msg.c_str());
    return;
  }
  server->send(200, "application/json", "{\"id\":0}");
}

// POST /api/plugin-jobs/complete {id, claim, ok, result?} -> {ok}
// 409 when `claim` is stale: the lease expired and another runner re-claimed
// the job, so this late result must not overwrite that runner's.
void CrossPointWebServer::handlePluginJobComplete() {
  if (rejectIfPluginsDisabled()) return;
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const uint32_t id = req["id"] | 0;
  const uint32_t claim = req["claim"] | 0;
  for (auto& job : pluginJobs) {
    if (job.id != id) continue;
    if (job.state == JOB_DONE || job.state == JOB_ERROR) {
      server->send(200, "application/json", "{\"ok\":true}");
      return;
    }
    if (job.state != JOB_RUNNING) break;
    if (job.claim != claim) {
      LOG_INF("WEB", "Plugin job %u: stale completion ignored", (unsigned)id);
      server->send(409, "application/json", "{\"error\":\"stale claim\"}");
      return;
    }
    job.state = (req["ok"] | false) ? JOB_DONE : JOB_ERROR;
    job.updatedAt = millis();
    job.result[0] = '\0';
    if (!req["result"].isNull()) {
      if (measureJson(req["result"]) >= sizeof(job.result)) {
        strcpy(job.result, "{\"error\":\"result too large\"}");
      } else {
        serializeJson(req["result"], job.result, sizeof(job.result));
      }
    }
    LOG_INF("WEB", "Plugin job %u %s", (unsigned)id, job.state == JOB_DONE ? "done" : "failed");
    server->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  server->send(404, "application/json", "{\"error\":\"no such running job\"}");
}

// GET /api/plugin-jobs/status?id=<n> -> {id, state, result}
void CrossPointWebServer::handlePluginJobStatus() {
  if (rejectIfPluginsDisabled()) return;
  const uint32_t id = strtoul(server->arg("id").c_str(), nullptr, 10);
  static constexpr const char* STATE_NAMES[] = {"empty", "pending", "running", "done", "error"};
  for (auto& job : pluginJobs) {
    if (job.id != id || job.state == JOB_EMPTY) continue;
    const std::string msg = "{\"id\":" + std::to_string(id) + ",\"state\":\"" + STATE_NAMES[job.state] +
                            "\",\"result\":" + (job.result[0] ? job.result : "null") + "}";
    server->send(200, "application/json", msg.c_str());
    return;
  }
  // Unknown: never existed, or its slot was recycled after completion.
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"id\":%u,\"state\":\"unknown\",\"result\":null}", (unsigned)id);
  server->send(200, "application/json", msg);
}

// WebSocket callback trampoline
