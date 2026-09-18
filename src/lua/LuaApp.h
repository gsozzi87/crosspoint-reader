#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
struct lua_State;

// ws397: apps en Lua desde la tarjeta.
//
// Los doce juegos que trae el aparato son Activities compiladas: agregar el
// trece es tocar el firmware, compilar y actualizar. La idea de rustmix-wave
// (que tiene su catálogo de juegos como paquetes Lua en la SD) es que eso no
// haga falta: se copia un archivo a la tarjeta por el modo memoria USB y
// aparece en el aparato.
//
// Una app es UN archivo: `/Apps/loquesea.lua`. Nada de carpetas, para que
// instalar sea copiar y desinstalar sea borrar.
//
// El contrato es de callbacks, no de bucle propio. El script define lo que
// quiera de esto:
//
//     function on_open()        -- una vez, al abrir
//     function on_key(k)        -- "up" "down" "ok" "back"; devolver true repinta
//     function on_tick()        -- cada TICK_MS; devolver true repinta
//     function on_draw()        -- pintar; la pantalla ya viene limpia
//
// Es a propósito: en tinta el refresco lo tiene que decidir el firmware (la
// regla del panel es un completo cada 10-15 parciales), y una app con su propio
// `while true` se comería el loop, los recordatorios y el reposo.
//
// El cajón: no hay `io`, ni `os`, ni `package`, ni `debug`, ni `require`, ni
// `load`. O sea que una app NO puede abrir archivos, ni salir a la red, ni
// tocar el I2C, ni el SPI del panel. Lo único que ve es la tabla `cp`, que está
// documentada en docs/ws397/APPS_LUA.md. Y todo lo que corre pasa por un worker
// de vida corta con el stack declarado (`tasks::runBounded`), así el stack de
// una app no vive en el loop de Arduino, que es el que anda justo.
//
// Las puertas (contrato v1, docs/ws397/PLAN_APPS_VIAJES_EPUB.md): micrófono,
// servidor, descargas, archivos propios, el visor y el lector. Todo lo que
// espera es ASÍNCRONO: `cp.listen`, `cp.call`, `cp.download`, `cp.view` y
// `cp.open_book` sólo ENCOLAN un pedido y vuelven en el acto; el host
// (LuaAppsActivity) lo saca de la cola desde su loop(), hace el trabajo con sus
// propias pantallas y le contesta a la app por `on_heard(texto)` /
// `on_reply(id, ok, tabla)`. La red y el micrófono NUNCA corren en el worker de
// Lua: son 32 KB de stack y el TLS no entra ahí.
class LuaApp {
 public:
  // Cuánto stack se le da al worker donde corre el script. 32 KB es lo mismo
  // que le da rustmix a su cargador de Lua.
  static constexpr uint32_t STACK = 32768;
  // Techo de memoria del intérprete. Sale de PSRAM (sobra) pero con tope, o una
  // tabla que crece sin parar se lleva puesto todo lo demás.
  static constexpr size_t MEM_CAP = 192 * 1024;
  static constexpr size_t SCRIPT_CAP = 64 * 1024;
  // Native drawing/logging lives outside Lua's allocator, so it needs its own
  // hard caps as well.
  static constexpr size_t TEXT_CAP = 512;
  static constexpr size_t LOG_CAP = 1024;
  // Instrucciones antes de cortar una llamada. Un `while true do end` tiene que
  // terminar en un error de la app, no en un aparato colgado.
  static constexpr int STEP_LIMIT = 400000;
  static constexpr unsigned long TICK_MS = 120;
  // Tope del texto que una app puede guardar (`cp.save`).
  static constexpr size_t SAVE_CAP = 4096;
  // Los archivos propios de la app (`/Apps/data/<app>/`): lo que se lee de un
  // tirón, lo que se escribe y lo que se manda al visor. Son topes de memoria,
  // no de tarjeta: `cp.read` con rango sirve para archivos más grandes.
  static constexpr size_t READ_CAP = 48 * 1024;
  static constexpr size_t WRITE_CAP = 64 * 1024;
  static constexpr size_t VIEW_CAP = 64 * 1024;
  // El JSON de `args` de `cp.call` y su profundidad. 16 KB es más que cualquier
  // índice de capítulos; una tabla cíclica se corta por la profundidad.
  static constexpr size_t ARGS_CAP = 16 * 1024;
  static constexpr int ARGS_DEPTH = 6;
  // Pedidos encolados como mucho. Salen de a uno y en orden.
  static constexpr int QUEUE_CAP = 4;
  // Nombres de archivo que pasa la app: [A-Za-z0-9._-]{1,48}, sin punto inicial.
  static constexpr size_t NAME_CAP = 48;
  // Segundos de escucha como mucho (VoiceRecorder reserva PSRAM por segundo).
  static constexpr int LISTEN_MAX_S = 30;

  // Un pedido de la app al host. Los strings significan distinto según el tipo:
  //   Listen:   seconds, a = pregunta (puede estar vacía)
  //   Call:     id, a = servicio, b = args en JSON
  //   Download: id, a = fileId, b = nombre de destino, c = "app" | "books"
  //   View:     a = nombre, b = título
  //   OpenBook: a = nombre
  struct Request {
    enum class Kind : uint8_t { Listen, Call, Download, View, OpenBook };
    Kind kind = Kind::Call;
    int id = 0;
    int seconds = 0;
    std::string a;
    std::string b;
    std::string c;
  };

  // Una app instalada: el archivo, el nombre (el del archivo sin `.lua`), y lo
  // que se MUESTRA: título y descripción, sacados del comentario que abre el
  // archivo (`-- Reloj: la hora grande, la fecha debajo.`). Sin ese
  // comentario, el título es el nombre con mayúscula y no hay descripción.
  struct Entry {
    std::string path;
    std::string name;
    std::string title;
    std::string description;
  };
  static std::vector<Entry> installed();
  // Lee el título y la descripción de la primera línea de comentario del
  // archivo. Pura: `header` es esa línea (o las primeras). Expuesta para que
  // la lista y la app abierta usen la misma regla, y para probarla sin placa.
  static void titleFromHeader(const std::string& header, const std::string& stem, std::string& title,
                              std::string& description);
  static const char* dir();

  ~LuaApp();

  bool open(GfxRenderer& renderer, const std::string& path);
  void close();

  bool ok() const { return state_ != nullptr && error_.empty(); }
  const std::string& error() const { return error_; }
  const std::string& name() const { return name_; }
  // El título que se muestra en los cabezales (ver Entry::title).
  const std::string& title() const { return title_; }
  // El nombre saneado que da nombre a las carpetas y viaja al servidor como
  // `app` en cp.call (`librito.lua` → "librito").
  const std::string& appId() const { return dirName_; }
  bool quitRequested() const { return quit_; }

  // Las tres llamadas al script. Devuelven true si hay que repintar. Un error
  // adentro deja `error()` cargado y la app se da por terminada.
  bool onKey(const char* key);
  bool onTick();
  void onDraw();

  // --- Las puertas: lo que el host atiende -----------------------------------
  // Saca el pedido más viejo de la cola. False si no hay ninguno.
  bool takeRequest(Request& out);
  bool hasRequests() const;
  // El host dice si tiene un pedido EN CURSO (ya sacado de la cola): es lo que
  // `cp.busy()` responde además de mirar la cola.
  void setBusy(bool busy);
  // Cancela lo que sigue encolado: cada Call/Download recibe
  // `on_reply(id, false, {error="cancelado"})` y un Listen, `on_heard(nil)`.
  // Devuelve true si algún callback pidió repintar.
  bool cancelQueued();

  // Lo que vuelve a la app. Todos devuelven true si hay que repintar (y el
  // contrato dice que después de cada uno se repinta igual). Si la app no
  // define `on_heard` / `on_reply` no es error: se ignora.
  bool onHeard(const char* textOrNull);
  // `json` es el cuerpo del servidor tal cual. `ok` = parsea y trae ok=true; si
  // no parsea, la tabla lleva `error`.
  bool onReply(int id, const std::string& json);
  bool onReplyError(int id, const char* error);
  bool onReplyBytes(int id, size_t bytes);

  // Carpetas de la app: `/Apps/data/<app>` y `/Books/<app>`. `<app>` es el
  // nombre del archivo sin `.lua`, saneado a [A-Za-z0-9._-].
  std::string dataDir() const;
  std::string booksDir() const;
  // Nombre válido para un archivo que pasa la app: [A-Za-z0-9._-]{1,48}, sin
  // punto inicial (nada de `.state`, nada de `..`) y sin barras.
  static bool validName(const char* name);

 private:
  bool callback(const char* fn, const char* arg);
  // Igual que callback() pero con un empujador de argumentos arbitrario, que
  // corre DENTRO del worker (es donde se arma la tabla de la respuesta).
  bool callbackWith(const char* fn, int (*push)(lua_State*, void*), void* ctx);

  lua_State* state_ = nullptr;
  std::string error_;
  std::string name_;
  std::string title_;
  std::string dirName_;
  std::string path_;
  bool quit_ = false;
  bool hasTick_ = false;
};
