#pragma once

#include <cstddef>
#include <string>

// HTTP client for the device's own server (ws397 Hono backend). One place for
// what every server-backed feature needs and nothing should re-implement:
//
//  - base URL + device token from ServerCredentialStore, sent as
//    "Authorization: Bearer <token>" on every request;
//  - JSON in / JSON out over the firmware's TLS stack (SecureHttpClient);
//  - retries with backoff on transport failures, 429 and 5xx (never on other
//    4xx: those are the request's fault), with an X-Request-Id the server can
//    use to de-duplicate a retried POST it already applied;
//  - an offline queue on the SD card for POSTs that must not be lost when the
//    device has no network (reminders, sync). flushQueue() replays it in order
//    whenever WiFi is up; entries the server rejects with a 4xx are dropped.
//
// Calls are synchronous and block the calling task: run them from a network
// activity (WiFi is only up inside those) or a worker task, never from the
// render path.
class ServerClient {
 public:
  enum class Result {
    Ok,            // 2xx; body in Response
    Queued,        // postOrQueue: no network / transport failure, saved for later
    NoNetwork,     // WiFi not connected
    NoServer,      // no base URL configured (custom or build-time)
    NoToken,       // token required but not set
    Transport,     // could not connect / no response after retries
    Unauthorized,  // 401 / 403
    HttpError,     // any other non-2xx; status + body in Response
  };

  struct Response {
    int status = 0;
    std::string body;
  };

  static ServerClient& getInstance() {
    static ServerClient instance;
    return instance;
  }

  // GET base+path. auth=false skips the token (public endpoints such as
  // /firmware/latest, useful to tell "server down" from "token wrong").
  Result get(const std::string& path, Response& out, bool auth = true);
  // POST a JSON body (already serialized) to base+path with the token.
  Result postJson(const std::string& path, const std::string& json, Response& out);
  // postJson, and on NoNetwork/Transport the request is queued instead (Queued).
  Result postOrQueue(const std::string& path, const std::string& json, Response* out = nullptr);

  // Offline queue (SD, /.crosspoint/server-queue.json, bounded; the oldest
  // entry is dropped when full).
  bool enqueue(const std::string& path, const std::string& json);
  size_t queueSize();
  // Replays queued POSTs in order while the network holds. Returns how many
  // were delivered (or rejected by the server and dropped); stops at the first
  // transport failure so ordering is preserved. -1 when there is no network,
  // server or token to try with.
  int flushQueue(size_t maxItems = 32);

  static const char* resultName(Result r);
  static bool networkUp();

 private:
  ServerClient() = default;
  Result request(const char* method, const std::string& path, const std::string* json, bool auth, Response& out);
  Result requestOnce(const char* method, const std::string& url, const std::string* json, bool auth,
                     const std::string& requestId, Response& out);
  static std::string newRequestId();
};

#define SERVER_CLIENT ServerClient::getInstance()
