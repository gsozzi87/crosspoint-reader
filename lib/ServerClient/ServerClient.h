#pragma once

#include <cstddef>
#include <cstdint>
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
    Queued,        // postOrQueue: se guardó para más tarde (sin red, transporte, 429/5xx o 401)
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
  // timeoutMs = 0 keeps the default (20 s); slow endpoints (LLM answers) pass
  // their own, per socket operation.
  // auth=false para los endpoints que se usan ANTES de estar vinculado
  // (/api/pair/start): mandar un token que el servidor todavía no conoce
  // volvería 401 antes de llegar al handler.
  Result postJson(const std::string& path, const std::string& json, Response& out, uint32_t timeoutMs = 0,
                  bool auth = true);
  // POST a raw body (e.g. audio/wav) with the token. Retries like postJson.
  Result postBytes(const std::string& path, const char* contentType, const uint8_t* data, size_t len,
                   Response& out, uint32_t timeoutMs = 0);
  // postJson, and on NoNetwork/Transport the request is queued instead (Queued).
  Result postOrQueue(const std::string& path, const std::string& json, Response* out = nullptr,
                     uint32_t timeoutMs = 0);

  // Offline queue (SD, /.crosspoint/server-queue.json, bounded; the oldest
  // entry is dropped when full).
  bool enqueue(const std::string& path, const std::string& json);
  size_t queueSize();
  // Tira la cola entera sin reproducirla. Existe para cuando el aparato cambia
  // de cuenta: los POST pendientes llevan ids que son de la cuenta VIEJA (el
  // store del servidor numera desde 1 en cada cuenta), asi que reproducirlos
  // contra la nueva tilda o borra lo que le toco el mismo numero.
  void clearQueue();
  // Sube lo pendiente apenas hay red, sin esperar a una sincronización. Se
  // llama sola desde la primera petición de cada sesión de red; queda pública
  // por si alguna pantalla quiere forzarla.
  void flushOnConnect();
  // Replays queued POSTs in order while the network holds. Returns how many
  // were delivered (or rejected by the server and dropped); stops at the first
  // transport failure so ordering is preserved. -1 when there is no network,
  // server or token to try with.
  int flushQueue(size_t maxItems = 32);

  static const char* resultName(Result r);
  static bool networkUp();

 private:
  bool inFlush_ = false;            // flushQueue() usa request(): no reentrar
  bool flushedThisSession_ = false;  // ya se vació en esta sesión de red
  ServerClient() = default;
  struct Body {
    const char* contentType = nullptr;
    const uint8_t* data = nullptr;
    size_t len = 0;
  };
  Result request(const char* method, const std::string& path, const Body* body, bool auth, Response& out,
                 uint32_t timeoutMs = 0);
  Result requestOnce(const char* method, const std::string& url, const Body* body, bool auth,
                     const std::string& requestId, Response& out, uint32_t timeoutMs = 0);
  static std::string newRequestId();
};

#define SERVER_CLIENT ServerClient::getInstance()
