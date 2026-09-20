#include "ServerClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <Logging.h>
#include <NetPump.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <esp_random.h>
#include <ws397_version.h>  // ws397: build number lives here, not in a -D flag

#include "QueueAccount.h"
#include "ServerCredentialStore.h"
#include "ServerErrorText.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureClient.h>
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

constexpr unsigned long SLOW_MS = 15000;  // más que esto se anota aunque haya salido bien

// REV-048: hasta donde se mira el cuerpo de un error y cuanto se muestra.
constexpr size_t ERROR_BODY_MAX = 2048;  // un error nuestro es JSON corto
constexpr size_t ERROR_TEXT_MAX = 120;   // lo que entra en un renglon del visor

// SON DOS PREGUNTAS DISTINTAS, y estaban contestadas con el mismo predicado.
//
//   1. ¿lo reintento AHORA, con el backoff de 500/1500 ms?
//   2. ¿esto puede andar MÁS TARDE, o sea va a la cola offline?
//
// Para un 429 las respuestas son opuestas: NO ahora —los topes que lo producen,
// el mensual de la cuenta y el del proveedor de IA, se miden en minutos o en un
// mes, no en un segundo y medio— y SÍ más tarde. Con una sola función, tener
// las dos bien era imposible: con el 429 adentro, un límite de uso de Groq
// costaba TRES llamadas condenadas, seis segundos de "Pensando" y tres
// mordiscos al cupo en vez de uno; sacándolo de las dos se perdía la operación
// encolada, que es justo lo que arregló F02.
bool retryable(int status) { return status < 0 || (status >= 500 && status <= 599); }

// La segunda pregunta. Incluye el 429 a propósito (ver arriba).
bool retryableLater(int status) { return retryable(status) || status == 429; }

std::string joinUrl(const std::string& base, const std::string& path) {
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}
}  // namespace

bool ServerClient::networkUp() { return WiFi.status() == WL_CONNECTED; }

const char* ServerClient::resultName(Result r) {
  switch (r) {
    case Result::Ok:
      return "ok";
    case Result::Queued:
      return "queued";
    case Result::NoNetwork:
      return "no network";
    case Result::NoServer:
      return "no server url";
    case Result::NoToken:
      return "no token";
    case Result::Transport:
      return "transport error";
    case Result::Unauthorized:
      return "unauthorized";
    case Result::HttpError:
      return "http error";
  }
  return "?";
}

// REV-048: el motivo que manda el servidor, no solo el numero.
//
// Nuestros errores son JSON corto ({ok:false, error, code}) y ahi adentro esta
// lo unico que distingue un modelo que ya no existe de un proveedor caido o de
// una clave vencida. El servidor ya lo redacta (redactSecrets), asi que lo que
// llega se puede mostrar y loguear tal cual.
std::string ServerClient::errorText(const Response& resp) {
  const std::string& body = resp.body;
  const size_t start = body.find_first_not_of(" \r\n\t");
  if (start == std::string::npos || body[start] != '{') return {};
  // Un cuerpo grande no es un error nuestro (y parsearlo costaria heap del
  // escaso justo cuando la peticion ya salio mal).
  if (body.size() - start > ERROR_BODY_MAX) return {};
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str() + start, body.size() - start) != DeserializationError::Ok) return {};
  const char* err = doc["error"] | "";
  if (!*err) return {};
  return servererr::tidy(err, ERROR_TEXT_MAX);
}

std::string ServerClient::describeFailure(Result r, const Response& resp) {
  std::string out = resultName(r);
  out += " (";
  out += std::to_string(resp.status);
  out += ")";
  const std::string why = errorText(resp);
  if (!why.empty()) {
    out += ": ";
    out += why;
  }
  return out;
}

std::string ServerClient::newRequestId() {
  char buf[17];
  const uint32_t a = esp_random();
  const uint32_t b = esp_random();
  snprintf(buf, sizeof(buf), "%08lx%08lx", static_cast<unsigned long>(a), static_cast<unsigned long>(b));
  return buf;
}

ServerClient::Result ServerClient::requestOnce(const char* method, const std::string& url, const Body* body, bool auth,
                                               const std::string& requestId, Response& out, uint32_t timeoutMs) {
  out.status = 0;
  out.body.clear();
#if defined(FREEINK_NET_WOLFSSL)
  // Keepalive de TCP en cada conexión al servidor (parche 0025 del SDK): un
  // router que deja de entregar la bajada dejaba la petición esperando el
  // tope entero (40-90 s) sobre una conexión que el servidor ya había
  // contestado. Con esto la conexión muda se cae a los 5 + 3·3 = 14 s y el
  // reintento de abajo sale por una conexión nueva, que en los logs siempre
  // anduvo en 3-4 s. Un servidor lento pero vivo contesta las sondas y no lo
  // dispara. Es un ajuste global del SDK: se pone una vez.
  static bool keepAliveSet = false;
  if (!keepAliveSet) {
    freeink::SecureClient::setKeepAlive(5, 3, 3);
    keepAliveSet = true;
  }
  freeink::SecureHttpClient http;
  http.setTimeout(timeoutMs ? timeoutMs : TIMEOUT_MS);
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
  // EL CUERPO SE ESCRIBE DERECHO Y EL LAZO DE ESPERA BOMBEA. El `sendRequest`
  // de tres argumentos junta el cuerpo adentro de `_body` y después había que
  // COPIARLO a `out.body` (dos copias del mismo cuerpo en el heap interno, que
  // es el escaso); y, sobre todo, no acepta `AbortCallback`, que es el
  // predicado que `SecureHttpClient` consulta en cada vuelta de sus lazos de
  // lectura. Con la versión de cinco argumentos el cuerpo cae directo en
  // `out.body` y, mientras el servidor piensa, el loop sigue atendiendo a PWR
  // y a Atrás. Los reintentos internos del SDK ocurren ANTES de que llegue un
  // solo byte de cuerpo (una escritura fallida o una línea de estado que no
  // llega sobre una conexión reusada), así que `out.body` no se puede
  // concatenar consigo mismo.
  const freeink::SecureHttpClient::DataCallback sink = [&out](const uint8_t* data, const size_t len) {
    out.body.append(reinterpret_cast<const char*>(data), len);  // binary-safe (/api/voice trae JSON + audio)
    return true;
  };
  const freeink::SecureHttpClient::AbortCallback pump = []() { return netpump::pumpAndCheckCancel(); };
  int status;
  if (body && body->data) {
    http.addHeader("Content-Type", body->contentType ? body->contentType : "application/octet-stream");
    status = http.sendRequest(method, body->data, body->len, sink, pump);
  } else {
    status = http.sendRequest(method, nullptr, 0, sink, pump);
  }
  out.status = status;
  // UNA PETICIÓN CORTADA NO ES UN 200. `sendRequestOnce` devuelve el estado que
  // alcanzó a leer aunque el cuerpo haya quedado por la mitad, así que sin esto
  // una cancelación a mitad de la respuesta se le entregaría al llamador como
  // un éxito con el cuerpo truncado.
  if (http.aborted()) {
    LOG_INF(TAG, "%s: cortada por el usuario (%s) con %u bytes recibidos", url.c_str(), netpump::cancelReason(),
            (unsigned)out.body.size());
    out.status = -1;
    out.body.clear();
    http.end();
    return Result::Transport;
  }
  http.end();
#else
  (void)method;
  (void)url;
  (void)body;
  (void)auth;
  (void)requestId;
  (void)timeoutMs;
  LOG_ERR(TAG, "no TLS client in this build");
  return Result::Transport;
#endif
  if (out.status < 0) return Result::Transport;
  if (out.status == 401 || out.status == 403) return Result::Unauthorized;
  if (out.status >= 200 && out.status <= 299) return Result::Ok;
  return Result::HttpError;
}

// LO PENDIENTE SUBE APENAS HAY RED, sin esperar a una sincronización.
//
// Antes la cola offline sólo se vaciaba en HubSyncActivity. O sea que un
// recordatorio borrado en el aparato sin WiFi seguía apareciendo en la nube
// hasta la próxima sincronización, aunque en el medio se hubiera usado Hablar,
// la Biblia o las Noticias, que levantan la red igual. Ahora la primera llamada
// de cada sesión de red vacía la cola antes de lo suyo: son unos pocos POST de
// un par de cientos de bytes y el orden queda bien (primero lo que el aparato
// hizo, después lo que se va a pedir).
// El reloj en hora ANTES del primer TLS, una vez por sesión de red.
//
// Hace falta para poder verificar el certificado del servidor: la validez se
// comprueba contra el reloj del SISTEMA, y en 1970 todo certificado del mundo
// parece "todavía no válido". El RTC ya lo cubre en el arranque
// (`HalClock::applyToSystemClock`), así que esto es sólo para el caso que el
// RTC no puede cubrir: un aparato recién armado, o uno que estuvo sin batería,
// donde el RTC tampoco sabe qué hora es. Ahí la única fuente es la red.
void ServerClient::ensureClockForTls() {
  if (clockCheckedThisSession_) return;
  clockCheckedThisSession_ = true;
  if (HalClock::systemClockLooksSet()) return;
  LOG_ERR(TAG, "el reloj no está en hora y hay que abrir TLS: se pide por NTP");
  if (halClock.syncFromNTP()) {
    halClock.applyToSystemClock();
  } else {
    LOG_ERR(TAG, "NTP tampoco contestó: el reloj sigue sin hora");
  }
}

void ServerClient::flushOnConnect() {
  if (inFlush_) return;
  if (holdFlush_) return;  // se está averiguando de qué cuenta es el aparato
  if (!networkUp()) {
    flushedThisSession_ = false;  // la próxima vez que haya red se vuelve a intentar
    clockCheckedThisSession_ = false;
    identity_ = Identity::Unknown;  // otra sesión de red, se vuelve a confirmar
    return;
  }
  if (flushedThisSession_) return;
  flushedThisSession_ = true;
  if (queueSize() == 0) return;
  inFlush_ = true;
  const int done = flushQueue();
  inFlush_ = false;
  LOG_INF(TAG, "al conectarse se subieron %d pendientes", done);
}

ServerClient::Result ServerClient::request(const char* method, const std::string& path, const Body* body, bool auth,
                                           Response& out, uint32_t timeoutMs) {
  if (!networkUp()) {
    flushedThisSession_ = false;
    clockCheckedThisSession_ = false;
    identity_ = Identity::Unknown;
    return Result::NoNetwork;
  }
  ensureClockForTls();
  flushOnConnect();
  const std::string base = SERVER_STORE.getBaseUrl();
  if (base.empty()) return Result::NoServer;
  if (auth && !SERVER_STORE.hasToken()) return Result::NoToken;

  const std::string url = joinUrl(base, path);
  // EL TRABAJO EN CURSO TIENE NOMBRE, y el bombeo empieza acá: la Scope ceba
  // el estado de los botones (el Atrás que YA estaba apretado cuando la
  // petición arrancó no cancela nada) y le da a la línea de "pasada larga" del
  // log algo que decir. Cubre los tres intentos, no cada uno: cancelar es de
  // la petición entera.
  netpump::Scope scope(path.c_str());
  // One id across the retries of a request: a server that applied the first
  // attempt but lost the response can recognise the replay.
  const std::string requestId = newRequestId();
  // Lo lee `postOrQueue()` si esto termina yendo a la cola. Se asigna DESPUÉS
  // de `flushOnConnect()` (que también hace peticiones), así que el que queda
  // es el de esta petición.
  lastRequestId_ = requestId;
  // SIN AHORRO DE ENERGÍA DEL WIFI MIENTRAS HAY UNA PETICIÓN EN CURSO. Con el
  // modem sleep puesto (el default), en algunos routers domésticos —el
  // INFINITUM del dueño, no el otro— una de cada dos conexiones nuevas se
  // quedaba muda hasta el tope (90 s en Hablar, 20-30 s en el hub) y el
  // reintento salía en 3 s. La OTA, que ya pone WIFI_PS_NONE, nunca se
  // trabó en esa misma red, y SpeechToText hace exactamente esto desde
  // siempre. Cuesta unos 70 mA sólo mientras dura la petición.
  const bool wifiDormia = WiFi.getSleep();
  if (wifiDormia) WiFi.setSleep(false);
  Result result = Result::Transport;
  for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
    if (attempt > 0) {
      LOG_DBG(TAG, "%s %s: retry %d after status %d", method, path.c_str(), attempt, out.status);
      // El backoff es una espera NUESTRA, así que también bombea: un segundo y
      // medio de delay() a secas entre dos intentos es un segundo y medio sin
      // atender a nadie, y justo cuando el dueño está apretando todo porque
      // "no pasa nada".
      netpump::pumpDelay(BACKOFF_MS[attempt - 1]);
      if (!networkUp()) {
        if (wifiDormia) WiFi.setSleep(true);
        return Result::NoNetwork;
      }
    }
    const unsigned long t0 = millis();
    result = requestOnce(method, url, body, auth, requestId, out, timeoutMs);
    const unsigned long took = millis() - t0;
    // UN INTENTO QUE FALLA O TARDA TIENE QUE DECIR CUÁNTO Y EN QUÉ ESTADO. Un
    // "retry 1 after status -1" a secas no distingue un servidor caído de un
    // WiFi que se cayó en el medio ni de un enlace que gotea: el dueño esperó
    // 90 s mirando "pensando" y el log no tenía con qué explicarlo.
    if (out.status < 0 || took >= SLOW_MS) {
      LOG_ERR(TAG, "%s %s: intento %d %s tras %lu ms (status %d, %u bytes subidos, wifi=%s rssi=%d dBm, heap %u KB)",
              method, path.c_str(), attempt + 1, out.status < 0 ? "FALLÓ" : "lento", took, out.status,
              (unsigned)(body && body->data ? body->len : 0), networkUp() ? "arriba" : "CAÍDO", (int)WiFi.RSSI(),
              (unsigned)(ESP.getFreeHeap() / 1024));
    }
    // UN SERVIDOR QUE TARDA NO SE REINTENTA. Un -1 justo al cumplirse el tope
    // es un servidor VIVO que todavía está trabajando (una pregunta con
    // búsqueda en internet son 20-60 s de modelo): repetir el pedido es
    // hacerle repetir el trabajo y esperar el tope otra vez —tres veces, dos
    // minutos de "Pensando" con el aparato sordo, que es lo que el dueño vio
    // como "se trabó"—. La conexión MUDA, que era el motivo del reintento, ya
    // no llega hasta acá: el keepalive (1.5.103) la corta a los ~14 s y ese -1
    // sí se reintenta, porque llega ANTES del tope.
    const uint32_t tope = timeoutMs ? timeoutMs : TIMEOUT_MS;
    if (out.status < 0 && took >= tope) {
      LOG_ERR(TAG,
              "%s %s: venció el tope de %lu ms sin que la conexión se cayera: el servidor sigue trabajando, no se "
              "reintenta",
              method, path.c_str(), (unsigned long)tope);
      break;
    }
    // CANCELADA ES CANCELADA: no se reintenta. Si el dueño apretó Atrás o PWR
    // para sacarse de encima una petición que no volvía, repetirla dos veces
    // más con su backoff es exactamente lo que él estaba tratando de evitar.
    if (netpump::cancelRequested()) {
      LOG_INF(TAG, "%s %s: cancelada por %s, no se reintenta", method, path.c_str(), netpump::cancelReason());
      break;
    }
    if (!retryable(out.status)) break;
  }
  if (wifiDormia) WiFi.setSleep(true);
  if (result != Result::Ok) {
    LOG_ERR(TAG, "%s %s -> %s", method, path.c_str(), describeFailure(result, out).c_str());
  }
  return result;
}

ServerClient::Result ServerClient::get(const std::string& path, Response& out, bool auth) {
  return request("GET", path, nullptr, auth, out);
}

ServerClient::Result ServerClient::postJson(const std::string& path, const std::string& json, Response& out,
                                            uint32_t timeoutMs, bool auth) {
  const Body body{"application/json", reinterpret_cast<const uint8_t*>(json.data()), json.size()};
  return request("POST", path, &body, auth, out, timeoutMs);
}

ServerClient::Result ServerClient::postBytes(const std::string& path, const char* contentType, const uint8_t* data,
                                             size_t len, Response& out, uint32_t timeoutMs) {
  const Body body{contentType, data, len};
  return request("POST", path, &body, true, out, timeoutMs);
}

ServerClient::Result ServerClient::postOrQueue(const std::string& path, const std::string& json, Response* out,
                                               const uint32_t timeoutMs) {
  Response local;
  Response& resp = out ? *out : local;
  const Result r = timeoutMs > 0 ? postJson(path, json, resp, timeoutMs) : postJson(path, json, resp);
  // Se encola TODO lo que puede andar más tarde, no sólo la falta de red.
  //
  // Antes sólo NoNetwork y Transport iban a la cola: un 503 del servidor, un
  // 429 por exceso de pedidos o un 401 porque el token todavía no está
  // vinculado devolvían error y la operación se perdía ahí mismo, mientras la
  // pantalla ya la había dado por hecha. El 401 es el caso más doloroso, porque
  // es transitorio por definición: el aparato recupera el acceso vinculándose,
  // y lo que se hizo mientras tanto tendría que seguir estando.
  const bool puedeAndarDespues = r == Result::NoNetwork || r == Result::Transport || r == Result::Unauthorized ||
                                 (r == Result::HttpError && retryableLater(resp.status));
  if (puedeAndarDespues) {
    // Con el id del intento en línea: la operación lógica es UNA y conserva su
    // id desde el primer intento hasta el replay de la cola.
    return enqueue(path, json, lastRequestId_) ? Result::Queued : r;
  }
  return r;
}

// ---- offline queue -------------------------------------------------------
// {"items":[{"id":"...","path":"/api/x","body":"{...}"}]} on the SD card. Small
// and rewritten whole: it is a safety net for a handful of requests, not a log.

bool ServerClient::enqueue(const std::string& path, const std::string& json, const std::string& requestId) {
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
  // EL ID VIENE DEL PRIMER INTENTO, no se inventa uno nuevo (REV-016). Si el
  // servidor alcanzó a aplicar el POST y la respuesta se perdió, el replay de
  // la cola llega con el MISMO `X-Request-Id` y el servidor lo reconoce en vez
  // de aplicarlo otra vez. Con un id nuevo, la nota o el recordatorio quedaban
  // duplicados y no había forma de saberlo desde ningún lado.
  item["id"] = requestId.empty() ? newRequestId() : requestId;
  // DE QUÉ CUENTA ES ESTA ENTRADA (REV-017). Es la segunda línea de defensa: el
  // portón está en `flushQueue()`, pero si `clearQueue()` no pudo escribir la
  // tarjeta —y una tarjeta que no escribe es un caso real acá— las entradas
  // viejas siguen en el archivo. Con el sello, igual no salen.
  if (!account_.empty()) item["acct"] = account_;
  item["path"] = path;
  item["body"] = json;
  const bool ok = PersistableStoreBase::writeDocToFile(QUEUE_PATH, doc);
  LOG_DBG(TAG, "queued POST %s (%u pending)", path.c_str(), (unsigned)items.size());
  return ok;
}

void ServerClient::clearQueue() {
  const size_t had = queueSize();
  if (had == 0) return;
  JsonDocument doc;
  doc["items"].to<JsonArray>();
  PersistableStoreBase::writeDocToFile(QUEUE_PATH, doc);
  LOG_INF(TAG, "cola descartada: %u pendientes de otra cuenta", static_cast<unsigned>(had));
}

size_t ServerClient::queueSize() {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(QUEUE_PATH, doc)) return 0;
  return doc["items"].is<JsonArray>() ? doc["items"].as<JsonArray>().size() : 0;
}

// De quién es el aparato AHORA. Devuelve si se pudo averiguar.
//
// `/api/pair/status` no necesita que el aparato esté vinculado: en un servidor
// de una sola cuenta contesta `single:true` con `account` nulo, y ahí no hay
// nada que comparar. En uno multiusuario devuelve el correo de la cuenta dueña
// del token.
//
// Si cambió, la cola de la cuenta anterior se TIRA (sus ids no significan nada
// en la cuenta nueva) y se avisa al resto del firmware para que suelte lo que
// tenga cacheado.
bool ServerClient::confirmAccount() {
  if (identity_ == Identity::Confirmed) return true;
  if (inConfirm_) return false;  // no reentrar: esto hace una petición

  inConfirm_ = true;
  // La pregunta no puede disparar la subida que está por autorizar:
  // `request()` llama a `flushOnConnect()` antes de mandar nada.
  const bool holdAnterior = holdFlush_;
  holdFlush_ = true;
  Response resp;
  const Result r = get("/api/pair/status", resp);
  holdFlush_ = holdAnterior;
  inConfirm_ = false;

  if (r != Result::Ok) {
    LOG_ERR(TAG, "no se pudo saber de qué cuenta es el aparato (status %d): la cola espera", resp.status);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    LOG_ERR(TAG, "/api/pair/status contestó algo que no es JSON: la cola espera");
    return false;
  }
  const char* acc = doc["account"] | "";
  const std::string ahora(acc ? acc : "");
  if (ahora != account_) {
    LOG_INF(TAG, "el aparato cambió de cuenta: se descarta lo de la anterior");
    // Queda anotado para toda la sesión: si `clearQueue()` no pudo escribir la
    // tarjeta, las entradas SIN sellar tampoco se pueden dar por buenas.
    accountChangedThisSession_ = true;
    clearQueue();
    account_ = ahora;
    if (onAccountChanged_) onAccountChanged_(account_.c_str());
  }
  identity_ = Identity::Confirmed;
  return true;
}

int ServerClient::flushQueue(size_t maxItems) {
  if (holdFlush_) {
    LOG_INF(TAG, "la cola espera: se está averiguando de qué cuenta es el aparato");
    return 0;
  }
  if (!networkUp()) return -1;
  const std::string base = SERVER_STORE.getBaseUrl();
  if (base.empty() || !SERVER_STORE.hasToken()) return -1;

  // Primero, ¿hay algo? Es una lectura de la tarjeta y nada más: con la cola
  // vacía —el caso normal— no se paga ninguna petición de red.
  {
    JsonDocument sonda;
    if (!PersistableStoreBase::readDocFromFile(QUEUE_PATH, sonda) || !sonda["items"].is<JsonArray>()) return 0;
    if (sonda["items"].as<JsonArray>().size() == 0) return 0;
  }

  // EL PORTÓN, y está acá para que valga en los TRES caminos que vacían la
  // cola: `flushOnConnect()` (la primera petición de cualquier sesión de red),
  // `devicesync::ifDue()` y la sincronización del hub. Puesto en una pantalla
  // dejaba los otros dos abiertos, que es lo que pasaba.
  //
  // Si no se puede averiguar, la cola espera a la próxima sesión, igual que
  // cuando no hay red: perder una vuelta no cuesta nada, aplicarla sobre la
  // cuenta equivocada le borra cosas a otro.
  if (!confirmAccount()) return 0;

  // Y RECIÉN ACÁ se lee la cola de verdad. El orden importa: `confirmAccount()`
  // puede haber llamado a `clearQueue()`, que vacía el ARCHIVO — una lista
  // leída antes seguiría viva en memoria y el bucle la mandaría igual. Es el
  // agujero que encontró el revisor sobre la primera versión de esto.
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(QUEUE_PATH, doc) || !doc["items"].is<JsonArray>()) return 0;
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() == 0) return 0;

  int done = 0;
  bool changed = false;
  while (items.size() > 0 && static_cast<size_t>(done) < maxItems) {
    // Se pregunta ACÁ y no después del envío: así el ítem que SÍ se entregó
    // antes de que el dueño cancelara se saca igual de la cola (si no, volvía
    // a subirse en la próxima sincronización), y el siguiente ni se intenta.
    if (netpump::cancelRequested()) {
      LOG_INF(TAG, "queue: se corta por %s, quedan %u pendientes", netpump::cancelReason(), (unsigned)items.size());
      break;
    }
    JsonObjectConst item = items[0].as<JsonObjectConst>();
    const std::string path = item["path"] | "";
    const std::string body = item["body"] | "";
    const std::string id = item["id"] | "";
    // Segunda línea de defensa: la regla vive en `queueacct` (un header puro,
    // probado de escritorio) para que sea UNA sola y no un `if` con una
    // premisa escrita al lado. Cubre el caso en que `clearQueue()` no pudo
    // escribir la tarjeta, y el de una entrada sin sellar de un firmware
    // anterior justo después de un cambio de cuenta.
    queueacct::Entry entrada;
    entrada.sealed = item["acct"].is<const char*>();
    if (entrada.sealed) entrada.account = item["acct"] | "";
    const queueacct::Session sesion{account_, accountChangedThisSession_};
    if (const char* porque = queueacct::refusal(entrada, sesion)) {
      LOG_ERR(TAG, "queue: se descarta %s, %s", path.c_str(), porque);
      items.remove(0);
      changed = true;
      continue;
    }
    Response resp;
    Result r = Result::Transport;
    // The queued id is the request id, so a replay after a lost response is
    // recognisable server-side.
    // La cola se vacía adentro de UNA pasada del loop: sin Scope no habría
    // bombeo (tick() no hace nada fuera de una) y cincuenta POST encolados
    // serían cincuenta esperas sordas seguidas.
    netpump::Scope scope(path.c_str());
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
      if (attempt > 0) netpump::pumpDelay(BACKOFF_MS[attempt - 1]);
      if (!networkUp()) {
        r = Result::NoNetwork;
        break;
      }
      const Body payload{"application/json", reinterpret_cast<const uint8_t*>(body.data()), body.size()};
      r = requestOnce("POST", joinUrl(base, path), &payload, true, id.empty() ? newRequestId() : id, resp);
      if (netpump::cancelRequested()) break;
      if (!retryable(resp.status)) break;  // un 429 no se reintenta en el acto…
    }
    if (r == Result::Ok) {
      items.remove(0);
      changed = true;
      ++done;
      continue;
    }
    // Sólo se tira lo que NUNCA va a andar: un 4xx definitivo (cuerpo mal
    // formado, ítem que ya no existe). Un 429, un 5xx o un 401 se conservan: el
    // servidor puede estar saturado, caído o el aparato sin vincular todavía, y
    // en los tres casos la operación sigue siendo válida. Antes se borraban los
    // tres y la acción desaparecía sin haber llegado nunca.
    // …pero TAMPOCO se tira: `retryableLater` es la que decide si la entrada
    // se conserva, y el 429 se conserva. Con `retryable` acá, un tope de uso
    // habría borrado la operación encolada, que es exactamente lo que arregló
    // F02.
    if (r == Result::HttpError && !retryableLater(resp.status)) {
      LOG_ERR(TAG, "queue: %s rechazado con %d (definitivo), se descarta", path.c_str(), resp.status);
      items.remove(0);
      changed = true;
      ++done;
      continue;
    }
    LOG_DBG(TAG, "queue: se detiene en %s (%s, estado %d): queda pendiente", path.c_str(), resultName(r), resp.status);
    break;
  }
  if (changed) PersistableStoreBase::writeDocToFile(QUEUE_PATH, doc);
  return done;
}
