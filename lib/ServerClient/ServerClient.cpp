#include "ServerClient.h"
#include <ws397_version.h>  // ws397: build number lives here, not in a -D flag

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <esp_random.h>

#include "ServerCredentialStore.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#endif

namespace {
constexpr const char* TAG = "SERVER";
constexpr const char* QUEUE_PATH = "/.crosspoint/server-queue.json";
constexpr size_t QUEUE_MAX_ITEMS = 50;
constexpr size_t QUEUE_MAX_BODY = 4096;
constexpr uint32_t TIMEOUT_MS = 20000;
constexpr int ATTEMPTS = 3;
constexpr uint32_t BACKOFF_MS[ATTEMPTS - 1] = {500, 1500};

bool retryable(int status) { return status < 0 || status == 429 || (status >= 500 && status <= 599); }

std::string joinUrl(const std::string& base, const std::string& path) {
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}
}  // namespace

bool ServerClient::networkUp() { return WiFi.status() == WL_CONNECTED; }

const char* ServerClient::resultName(Result r) {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::Queued: return "queued";
    case Result::NoNetwork: return "no network";
    case Result::NoServer: return "no server url";
    case Result::NoToken: return "no token";
    case Result::Transport: return "transport error";
    case Result::Unauthorized: return "unauthorized";
    case Result::HttpError: return "http error";
  }
  return "?";
}

std::string ServerClient::newRequestId() {
  char buf[17];
  const uint32_t a = esp_random();
  const uint32_t b = esp_random();
  snprintf(buf, sizeof(buf), "%08lx%08lx", static_cast<unsigned long>(a), static_cast<unsigned long>(b));
  return buf;
}

ServerClient::Result ServerClient::requestOnce(const char* method, const std::string& url, const std::string* json,
                                               bool auth, const std::string& requestId, Response& out) {
  out.status = 0;
  out.body.clear();
#if defined(FREEINK_NET_WOLFSSL)
  freeink::SecureHttpClient http;
  http.setTimeout(TIMEOUT_MS);
  // Same trust model as HttpDownloader's wolfSSL path.
  http.setInsecure();
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!http.begin(url)) {
    LOG_ERR(TAG, "bad URL: %s", url.c_str());
    return Result::Transport;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("X-Request-Id", requestId);
  if (auth) http.addHeader("Authorization", "Bearer " + SERVER_STORE.getToken());
  int status;
  if (json) {
    http.addHeader("Content-Type", "application/json");
    status = http.sendRequest(method, *json);
  } else {
    status = http.sendRequest(method, nullptr, 0);
  }
  out.status = status;
  out.body = http.getString();
  http.end();
#else
  (void)method;
  (void)url;
  (void)json;
  (void)auth;
  (void)requestId;
  LOG_ERR(TAG, "no TLS client in this build");
  return Result::Transport;
#endif
  if (out.status < 0) return Result::Transport;
  if (out.status == 401 || out.status == 403) return Result::Unauthorized;
  if (out.status >= 200 && out.status <= 299) return Result::Ok;
  return Result::HttpError;
}

ServerClient::Result ServerClient::request(const char* method, const std::string& path, const std::string* json,
                                           bool auth, Response& out) {
  if (!networkUp()) return Result::NoNetwork;
  const std::string base = SERVER_STORE.getBaseUrl();
  if (base.empty()) return Result::NoServer;
  if (auth && !SERVER_STORE.hasToken()) return Result::NoToken;

  const std::string url = joinUrl(base, path);
  // One id across the retries of a request: a server that applied the first
  // attempt but lost the response can recognise the replay.
  const std::string requestId = newRequestId();
  Result result = Result::Transport;
  for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
    if (attempt > 0) {
      LOG_DBG(TAG, "%s %s: retry %d after status %d", method, path.c_str(), attempt, out.status);
      delay(BACKOFF_MS[attempt - 1]);
      if (!networkUp()) return Result::NoNetwork;
    }
    result = requestOnce(method, url, json, auth, requestId, out);
    if (!retryable(out.status)) break;
  }
  if (result != Result::Ok) {
    LOG_ERR(TAG, "%s %s -> %s (status %d)", method, path.c_str(), resultName(result), out.status);
  }
  return result;
}

ServerClient::Result ServerClient::get(const std::string& path, Response& out, bool auth) {
  return request("GET", path, nullptr, auth, out);
}

ServerClient::Result ServerClient::postJson(const std::string& path, const std::string& json, Response& out) {
  return request("POST", path, &json, true, out);
}

ServerClient::Result ServerClient::postOrQueue(const std::string& path, const std::string& json, Response* out) {
  Response local;
  Response& resp = out ? *out : local;
  const Result r = postJson(path, json, resp);
  if (r == Result::NoNetwork || r == Result::Transport) {
    return enqueue(path, json) ? Result::Queued : r;
  }
  return r;
}

// ---- offline queue -------------------------------------------------------
// {"items":[{"id":"...","path":"/api/x","body":"{...}"}]} on the SD card. Small
// and rewritten whole: it is a safety net for a handful of requests, not a log.

bool ServerClient::enqueue(const std::string& path, const std::string& json) {
  if (json.size() > QUEUE_MAX_BODY) {
    LOG_ERR(TAG, "queue: body too large (%u bytes)", (unsigned)json.size());
    return false;
  }
  JsonDocument doc;
  PersistableStoreBase::readDocFromFile(QUEUE_PATH, doc);  // missing file = empty queue
  JsonArray items = doc["items"].is<JsonArray>() ? doc["items"].as<JsonArray>() : doc["items"].to<JsonArray>();
  while (items.size() >= QUEUE_MAX_ITEMS) {
    LOG_ERR(TAG, "queue full, dropping oldest entry");
    items.remove(0);
  }
  JsonObject item = items.add<JsonObject>();
  item["id"] = newRequestId();
  item["path"] = path;
  item["body"] = json;
  const bool ok = PersistableStoreBase::writeDocToFile(QUEUE_PATH, doc);
  LOG_DBG(TAG, "queued POST %s (%u pending)", path.c_str(), (unsigned)items.size());
  return ok;
}

size_t ServerClient::queueSize() {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(QUEUE_PATH, doc)) return 0;
  return doc["items"].is<JsonArray>() ? doc["items"].as<JsonArray>().size() : 0;
}

int ServerClient::flushQueue(size_t maxItems) {
  if (!networkUp()) return -1;
  const std::string base = SERVER_STORE.getBaseUrl();
  if (base.empty() || !SERVER_STORE.hasToken()) return -1;

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(QUEUE_PATH, doc) || !doc["items"].is<JsonArray>()) return 0;
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() == 0) return 0;

  int done = 0;
  bool changed = false;
  while (items.size() > 0 && static_cast<size_t>(done) < maxItems) {
    JsonObjectConst item = items[0].as<JsonObjectConst>();
    const std::string path = item["path"] | "";
    const std::string body = item["body"] | "";
    const std::string id = item["id"] | "";
    Response resp;
    Result r = Result::Transport;
    // The queued id is the request id, so a replay after a lost response is
    // recognisable server-side.
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
      if (attempt > 0) delay(BACKOFF_MS[attempt - 1]);
      if (!networkUp()) {
        r = Result::NoNetwork;
        break;
      }
      r = requestOnce("POST", joinUrl(base, path), &body, true, id.empty() ? newRequestId() : id, resp);
      if (!retryable(resp.status)) break;
    }
    if (r == Result::Ok || r == Result::HttpError || r == Result::Unauthorized) {
      if (r != Result::Ok) LOG_ERR(TAG, "queue: server rejected %s with %d, dropping", path.c_str(), resp.status);
      items.remove(0);
      changed = true;
      ++done;
      continue;
    }
    LOG_DBG(TAG, "queue: stopping at %s (%s)", path.c_str(), resultName(r));
    break;
  }
  if (changed) PersistableStoreBase::writeDocToFile(QUEUE_PATH, doc);
  return done;
}
