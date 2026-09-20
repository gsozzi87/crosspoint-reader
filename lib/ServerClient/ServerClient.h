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
  Result postBytes(const std::string& path, const char* contentType, const uint8_t* data, size_t len, Response& out,
                   uint32_t timeoutMs = 0);
  // postJson, and on NoNetwork/Transport the request is queued instead (Queued).
  Result postOrQueue(const std::string& path, const std::string& json, Response* out = nullptr, uint32_t timeoutMs = 0);

  // Offline queue (SD, /.crosspoint/server-queue.json, bounded; the oldest
  // entry is dropped when full).
  bool enqueue(const std::string& path, const std::string& json, const std::string& requestId = std::string());
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

  // ── Identidad de cuenta: de quién es lo que hay en la cola ────────────────
  //
  // La cola guarda POSTs con ids del STORE, y el servidor numera desde 1 en
  // CADA cuenta. Reproducir la cola de la cuenta A contra la B no duplica: le
  // tilda, le corre la fecha o le borra a B el objeto que casualmente tenga ese
  // número. Desde `/board` se puede mudar un aparato de cuenta sin tocarle el
  // token, así que el aparato no puede dar por hecho de quién es.
  //
  // La política vive ACÁ y no en una pantalla porque hay tres caminos que
  // vacían la cola —`flushOnConnect()` (la primera petición de cualquier
  // sesión), `devicesync::ifDue()` y la sincronización del hub— y una guardia
  // puesta en uno solo deja los otros dos abiertos.
  //
  // `account` es opaco para ServerClient: es lo que devuelve `/api/pair/status`
  // (el correo de la cuenta, o vacío en un servidor de una sola cuenta).
  void setAccount(const std::string& account) { account_ = account; }
  const std::string& account() const { return account_; }
  // Aviso al resto del firmware de que el aparato cambió de cuenta, para que
  // tire lo que tenga cacheado de la anterior. Puntero a función y no
  // std::function: esto vive en el camino de red, no en la UI.
  using AccountChangedFn = void (*)(const char* nuevaCuenta);
  void setAccountChangedHandler(AccountChangedFn fn) { onAccountChanged_ = fn; }

  // Pregunta de quién es el aparato AHORA. Devuelve si se pudo averiguar.
  // Es un GET de ~200 bytes y se hace una vez por sesión de red, y sólo
  // cuando hay algo encolado que mandar.
  bool confirmAccount();
  // Si la identidad de esta sesión de red ya está confirmada.
  bool accountConfirmed() const { return identity_ == Identity::Confirmed; }
  // Mientras esté puesto, NINGUNA petición vacía la cola de paso.
  //
  // Existe por un orden que dejaba muerta la guardia de cuenta (F11): el
  // chequeo de identidad es `GET /api/pair/status`, o sea una petición, y
  // `request()` llama a `flushOnConnect()` ANTES de mandar nada. La cola se
  // subía mientras se preguntaba de quién era el aparato, así que llegar a la
  // comparación ya era tarde. Quien pregunte por la identidad pone esto
  // primero.
  void setFlushHold(bool hold) { holdFlush_ = hold; }
  void ensureClockForTls();
  // Replays queued POSTs in order while the network holds. Returns how many
  // were delivered (or rejected by the server and dropped); stops at the first
  // transport failure so ordering is preserved. -1 when there is no network,
  // server or token to try with.
  int flushQueue(size_t maxItems = 32);

  static const char* resultName(Result r);

  // Lo que el servidor DIJO al fallar, no solo el numero.
  //
  // REV-048: `http error (502)` era el mismo cartel para un modelo que ya no
  // existe, un parametro rechazado, el proveedor caido, la clave vencida y el
  // STT roto. El servidor ya manda el motivo redactado en el JSON de error
  // ({ok:false, error, code}) y el aparato lo tiraba, asi que ni la pantalla ni
  // el log decian que corregir. `errorText` devuelve ese campo `error`
  // (vacio si el cuerpo no es un error nuestro) y `describeFailure` arma el
  // renglon completo que usan las pantallas y la linea de error de `request`.
  static std::string errorText(const Response& resp);
  static std::string describeFailure(Result r, const Response& resp);

  static bool networkUp();

 private:
  enum class Identity : uint8_t { Unknown, Confirmed };

  bool inFlush_ = false;  // flushQueue() usa request(): no reentrar
  bool flushedThisSession_ = false;
  bool clockCheckedThisSession_ = false;   // el reloj ya se miró en esta sesión de red
  bool holdFlush_ = false;                 // ver setFlushHold()
  bool inConfirm_ = false;                 // confirmAccount() usa request(): no reentrar
  Identity identity_ = Identity::Unknown;  // por SESIÓN de red, no por arranque
  std::string account_;                    // de qué cuenta se cree el aparato
  AccountChangedFn onAccountChanged_ = nullptr;
  // El id de la última petición de `request()`. Lo lee `postOrQueue()` para
  // encolar con el MISMO id con el que se intentó en línea (ver REV-016).
  std::string lastRequestId_;
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
