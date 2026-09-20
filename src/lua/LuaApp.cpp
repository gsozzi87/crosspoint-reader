#include "LuaApp.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "../HubStore.h"
#include "../TaskConfig.h"
#include "../activities/home/CalendarActivity.h"
#include "../input/MotionInput.h"
#include "../voice/UiSound.h"
#include "LuaSandbox.h"
#include "components/Selection.h"
#include "fontIds.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {
constexpr const char* TAG = "LUA";
constexpr const char* APPS_DIR = "/Apps";
constexpr const char* STATE_DIR = "/Apps/.state";
constexpr const char* EXT = ".lua";

// Una app por vez: el renderer y el contador de instrucciones son de la que
// esté corriendo. Es lo que permite que las funciones de `cp` sean C plano sin
// arrastrar un puntero por cada llamada.
GfxRenderer* g_renderer = nullptr;
bool g_quit = false;
std::string g_appStem;
char g_nativeText[LuaApp::TEXT_CAP + 1] = {};

// UN SOLO HILO ADENTRO DE LA VM (1.5.110). `on_draw` corre desde la tarea de
// render (ActivityManager::renderTaskLoop) y `on_tick`/`on_key` desde el loop de
// Arduino, y cada uno abre su propio worker sobre el MISMO lua_State. Sin
// candado, un tick de 120 ms caía en el medio de un on_draw de 275 ms y las dos
// llamadas pisaban la pila de Lua a la vez: el Librito murió con "librito:53:
// attempt to compare nil with number" en una línea donde el nil no puede venir
// de ningún lado (`cp.textw` devuelve siempre un entero). Recursivo porque
// cancelQueued() llama a onHeard()/onReplyError(), que vuelven a tomarlo.
SemaphoreHandle_t g_vmLock = xSemaphoreCreateRecursiveMutex();
struct VmGuard {
  VmGuard() { xSemaphoreTakeRecursive(g_vmLock, portMAX_DELAY); }
  ~VmGuard() { xSemaphoreGiveRecursive(g_vmLock); }
  VmGuard(const VmGuard&) = delete;
  VmGuard& operator=(const VmGuard&) = delete;
};
char g_logLine[LuaApp::LOG_CAP + 1] = {};

// La cola de pedidos al host y lo que `cp.busy()` responde. Viven acá y no en
// la instancia por lo mismo que el renderer: las funciones de `cp` son C plano.
// Las escribe el worker (adentro de una llamada de la app) y las lee el host
// DESPUÉS de que el worker terminó, así que no hay dos tareas encima a la vez.
std::deque<LuaApp::Request> g_requests;
bool g_hostBusy = false;
int g_nextId = 0;
// Carpetas de la app de turno, ya armadas (`/Apps/data/<app>`, `/Books/<app>`).
std::string g_dataDir;
std::string g_booksDir;
constexpr const char* DATA_ROOT = "/Apps/data";
constexpr const char* BOOKS_ROOT = "/Books";
constexpr const char* TMP_SUFFIX = ".tmp";

// Memoria para las cosas grandes (un archivo leído, el JSON de una respuesta):
// de PSRAM si se puede, que es donde hay lugar; la interna es la que el TLS y
// el parseo del EPUB necesitan y la que anda justa.
void* bigAlloc(const size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL);
  return p;
}

// ArduinoJson con la memoria en PSRAM, por lo mismo.
struct PsramAllocator final : ArduinoJson::Allocator {
  void* allocate(const size_t n) override { return bigAlloc(n); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, const size_t n) override {
    void* out = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    if (!out) out = heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL);
    return out;
  }
  static PsramAllocator& instance() {
    static PsramAllocator a;
    return a;
  }
};

const char* boundedText(lua_State* L, const int index) {
  size_t len = 0;
  const char* source = luaL_checklstring(L, index, &len);
  len = std::min(len, LuaApp::TEXT_CAP);
  len = static_cast<size_t>(utf8SafeTruncateBuffer(source, static_cast<int>(len)));
  std::memcpy(g_nativeText, source, len);
  g_nativeText[len] = '\0';
  return g_nativeText;
}

// --- La tabla cp ---------------------------------------------------------

int clampCoord(const lua_Integer v, const int extent) {
  if (v < -extent) return -extent;
  if (v > extent * 2) return extent * 2;
  return static_cast<int>(v);
}

int clampSize(const lua_Integer v, const int extent) {
  if (v <= 0) return 0;
  return static_cast<int>(std::min<lua_Integer>(v, extent));
}

int clampStroke(const lua_Integer v) { return static_cast<int>(std::max<lua_Integer>(1, std::min<lua_Integer>(v, 8))); }

int fontFor(const lua_Integer size) {
  if (size >= 14) return UI_14_FONT_ID;
  if (size <= 10) return UI_10_FONT_ID;
  return UI_12_FONT_ID;
}

int cpClear(lua_State*) {
  if (g_renderer) g_renderer->clearScreen();
  return 0;
}

int cpText(lua_State* L) {
  const int x = clampCoord(luaL_checkinteger(L, 1), g_renderer ? g_renderer->getScreenWidth() : 800);
  const int y = clampCoord(luaL_checkinteger(L, 2), g_renderer ? g_renderer->getScreenHeight() : 800);
  const char* text = boundedText(L, 3);
  const int font = fontFor(luaL_optinteger(L, 4, 12));
  const bool bold = lua_toboolean(L, 5) != 0;
  if (g_renderer) {
    g_renderer->drawText(font, x, y, text, true, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  return 0;
}

int cpTextWidth(lua_State* L) {
  const char* text = boundedText(L, 1);
  const int font = fontFor(luaL_optinteger(L, 2, 12));
  lua_pushinteger(L, g_renderer ? g_renderer->getTextWidth(font, text) : 0);
  return 1;
}

int cpTextHeight(lua_State* L) {
  const int font = fontFor(luaL_optinteger(L, 1, 12));
  lua_pushinteger(L, g_renderer ? g_renderer->getTextHeight(font) : 0);
  return 1;
}

int cpRect(lua_State* L) {
  const int screenW = g_renderer ? g_renderer->getScreenWidth() : 800;
  const int screenH = g_renderer ? g_renderer->getScreenHeight() : 800;
  const int x = clampCoord(luaL_checkinteger(L, 1), screenW);
  const int y = clampCoord(luaL_checkinteger(L, 2), screenH);
  const int w = clampSize(luaL_checkinteger(L, 3), screenW);
  const int h = clampSize(luaL_checkinteger(L, 4), screenH);
  const bool filled = lua_toboolean(L, 5) != 0;
  if (!g_renderer) return 0;
  if (filled) {
    g_renderer->fillRect(x, y, w, h, true);
  } else {
    g_renderer->drawRect(x, y, w, h, clampStroke(luaL_optinteger(L, 6, 1)), true);
  }
  return 0;
}

int cpLine(lua_State* L) {
  if (!g_renderer) return 0;
  const int screenW = g_renderer->getScreenWidth();
  const int screenH = g_renderer->getScreenHeight();
  g_renderer->drawLine(clampCoord(luaL_checkinteger(L, 1), screenW), clampCoord(luaL_checkinteger(L, 2), screenH),
                       clampCoord(luaL_checkinteger(L, 3), screenW), clampCoord(luaL_checkinteger(L, 4), screenH),
                       clampStroke(luaL_optinteger(L, 5, 1)), true);
  return 0;
}

// cp.image(x, y, w, h, bits [, escala]): un bitmap de 1 bit empaquetado por
// filas (MSB primero, 1 = tinta, cada fila redondeada a byte). Se pinta píxel a
// píxel con drawPixel para que la orientación del panel se aplique sola
// (drawImage del renderer no rota los bits). `escala` entera de 1 a 8 agranda
// cada píxel a un cuadrado. Existe para las apps con dibujitos (la mascota):
// 64 × 64 son 512 bytes, que caben de sobra en un string de Lua.
int cpImage(lua_State* L) {
  if (!g_renderer) return 0;
  const int screenW = g_renderer->getScreenWidth();
  const int screenH = g_renderer->getScreenHeight();
  const int x = clampCoord(luaL_checkinteger(L, 1), screenW);
  const int y = clampCoord(luaL_checkinteger(L, 2), screenH);
  const int w = static_cast<int>(luaL_checkinteger(L, 3));
  const int h = static_cast<int>(luaL_checkinteger(L, 4));
  size_t len = 0;
  const char* bits = luaL_checklstring(L, 5, &len);
  int scale = static_cast<int>(luaL_optinteger(L, 6, 1));
  if (scale < 1) scale = 1;
  if (scale > 8) scale = 8;
  if (w < 1 || h < 1 || w > 256 || h > 256) return luaL_error(L, "cp.image: tamaño fuera de rango (1-256)");
  const size_t stride = static_cast<size_t>((w + 7) / 8);
  if (len < stride * static_cast<size_t>(h))
    return luaL_error(L, "cp.image: faltan bytes (%d de %d)", (int)len, (int)(stride * h));
  const auto* p = reinterpret_cast<const uint8_t*>(bits);
  for (int row = 0; row < h; row++) {
    const int py = y + row * scale;
    if (py >= screenH) break;
    const uint8_t* line = p + row * stride;
    for (int col = 0; col < w; col++) {
      if (((line[col >> 3] >> (7 - (col & 7))) & 1) == 0) continue;
      const int px = x + col * scale;
      if (px >= screenW) break;
      if (scale == 1) {
        g_renderer->drawPixel(px, py, true);
      } else {
        // El bloque de una escala > 1 mide `scale` y hay que RECORTARLO, no
        // sólo mirar su esquina: con px = ancho-1 y escala 2 se escribía la
        // columna `ancho` entera, que es lo que llenaba el log de
        // "N pixeles fuera de pantalla (ultimo 480,…)" en cada cuadro.
        const int bw = screenW - px < scale ? screenW - px : scale;
        const int bh = screenH - py < scale ? screenH - py : scale;
        g_renderer->fillRect(px, py, bw, bh, true);
      }
    }
  }
  return 0;
}

// El resalte del sistema visual (docs/ws397/DISENO.md): pestaña, marco y
// franjas SÓLO en los márgenes. Se expone para que una app con una lista se vea
// como el resto del aparato en vez de inventar su propio negro macizo.
int cpSelection(lua_State* L) {
  if (!g_renderer) return 0;
  const int screenW = g_renderer->getScreenWidth();
  const int screenH = g_renderer->getScreenHeight();
  drawSelectionRow(*g_renderer, clampCoord(luaL_checkinteger(L, 1), screenW),
                   clampCoord(luaL_checkinteger(L, 2), screenH), clampSize(luaL_checkinteger(L, 3), screenW),
                   clampSize(luaL_checkinteger(L, 4), screenH), 0);
  return 0;
}

int cpWidth(lua_State* L) {
  lua_pushinteger(L, g_renderer ? g_renderer->getScreenWidth() : 0);
  return 1;
}

int cpHeight(lua_State* L) {
  lua_pushinteger(L, g_renderer ? g_renderer->getScreenHeight() : 0);
  return 1;
}

int cpMotion(lua_State* L) {
  const MotionInput::Event e = MOTION.takeAny();
  if (e == MotionInput::Event::None) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, MotionInput::name(e));
  }
  return 1;
}

int cpMs(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(millis()));
  return 1;
}

int cpBeep(lua_State* L) {
  const char* which = luaL_optstring(L, 1, "nav");
  uisound::Sound s = uisound::Sound::Nav;
  if (strcmp(which, "ok") == 0)
    s = uisound::Sound::Select;
  else if (strcmp(which, "back") == 0)
    s = uisound::Sound::Back;
  else if (strcmp(which, "error") == 0)
    s = uisound::Sound::Error;
  UI_SOUND.play(s);
  return 0;
}

int cpLog(lua_State* L) {
  const int n = lua_gettop(L);
  size_t used = 0;
  g_logLine[0] = '\0';
  for (int i = 1; i <= n; ++i) {
    if (i > 1 && used < LuaApp::LOG_CAP) g_logLine[used++] = ' ';
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    const size_t take = std::min(len, LuaApp::LOG_CAP - used);
    std::memcpy(g_logLine + used, s, take);
    used += take;
    lua_pop(L, 1);
    if (used == LuaApp::LOG_CAP) break;
  }
  used = static_cast<size_t>(utf8SafeTruncateBuffer(g_logLine, static_cast<int>(used)));
  g_logLine[used] = '\0';
  LOG_INF(TAG, "%s: %s", g_appStem.c_str(), g_logLine);
  return 0;
}

int cpQuit(lua_State*) {
  g_quit = true;
  return 0;
}

// La hora. Es la ÚNICA forma que tiene una app de saberla: `os` no está en el
// cajón (`os.execute` y `os.remove` vienen en la misma biblioteca), así que sin
// esto una agenda, un reloj o un juego por turnos no se podían escribir.
// Devuelve nil cuando el aparato todavía no está en hora, que es un estado real
// y frecuente: sin WiFi y sin haber sincronizado nunca, el RTC no sabe nada.
int cpTime(lua_State* L) {
  time_t epoch = 0;
  if (!halClock.getEpochUtc(epoch) || epoch <= 0) {
    lua_pushnil(L);
    return 1;
  }
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
  CalendarActivity::localFromEpoch(epoch, year, month, day, hour, minute);
  lua_createtable(L, 0, 8);
  const auto campo = [L](const char* nombre, const lua_Integer valor) {
    lua_pushinteger(L, valor);
    lua_setfield(L, -2, nombre);
  };
  campo("year", year);
  campo("month", month);
  campo("day", day);
  campo("hour", hour);
  campo("min", minute);
  // El segundo no sale de localFromEpoch: se saca del epoch, que es el mismo
  // instante.
  campo("sec", static_cast<lua_Integer>(epoch % 60));
  // 1 = lunes, 7 = domingo. weekdayOfCivil devuelve 0 = lunes.
  campo("wday", CalendarActivity::weekdayOfCivil(year, month, day) + 1);
  // El epoch va en UTC, que es lo que guarda el RTC: sirve para medir
  // diferencias entre dos llamadas sin pelearse con el huso.
  campo("epoch", static_cast<lua_Integer>(epoch));
  return 1;
}

// Lo único que una app puede escribir en la tarjeta: un archivo suyo, con su
// nombre, en /Apps/.state. No recibe rutas: no puede elegir dónde escribir.
int cpSave(lua_State* L) {
  size_t len = 0;
  const char* data = luaL_checklstring(L, 1, &len);
  if (len > LuaApp::SAVE_CAP) len = LuaApp::SAVE_CAP;
  // Es TEXTO, no binario: se corta en el primer cero para que lo que se guarda
  // sea exactamente lo que se lee después (writeFile toma una String, que
  // termina en cero igual). Cortarlo acá lo deja dicho en vez de que sorprenda.
  if (const void* nul = std::memchr(data, '\0', len)) len = static_cast<const char*>(nul) - data;
  Storage.ensureDirectoryExists(STATE_DIR);
  const std::string path = std::string(STATE_DIR) + "/" + g_appStem + ".txt";
  String out;
  out.concat(data, len);
  lua_pushboolean(L, Storage.writeFile(path.c_str(), out) ? 1 : 0);
  return 1;
}

int cpLoad(lua_State* L) {
  const std::string path = std::string(STATE_DIR) + "/" + g_appStem + ".txt";
  if (!Storage.exists(path.c_str())) {
    lua_pushnil(L);
    return 1;
  }
  const String content = Storage.readFile(path.c_str());
  lua_pushlstring(L, content.c_str(), content.length());
  return 1;
}

// --- Las puertas ---------------------------------------------------------
//
// Nada de esto hace el trabajo: encola un pedido y vuelve. El host lo saca de la
// cola desde su loop() y contesta por on_heard / on_reply. Ver LuaApp.h.

bool nameCharOk(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// Un identificador que viaja al servidor (servicio, fileId): mismo alfabeto que
// un nombre de archivo, con punto inicial permitido y hasta 64.
bool validToken(const char* s) {
  const size_t len = strlen(s);
  if (len == 0 || len > 64) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!nameCharOk(s[i])) return false;
  }
  return true;
}

bool queueFull() { return g_requests.size() >= static_cast<size_t>(LuaApp::QUEUE_CAP); }

std::string dataPath(const char* name) { return g_dataDir + "/" + name; }

// --- Lua -> JSON (los `args` de cp.call) ------------------------------------
//
// Tabla con claves string -> objeto; tabla secuencial 1..n -> array; tabla
// vacía -> objeto vacío. Números, booleanos y strings tal cual; nil dentro de
// un objeto se saltea (en un array no puede aparecer: cortaría la secuencia).
// La profundidad tiene tope porque una tabla que se referencia a sí misma no
// termina nunca.
bool isSequence(lua_State* L, const int idx) {
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
  lua_Integer count = 0;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    lua_pop(L, 1);
    if (!lua_isinteger(L, -1)) {
      lua_pop(L, 1);
      return false;
    }
    const lua_Integer k = lua_tointeger(L, -1);
    if (k < 1 || k > n) {
      lua_pop(L, 1);
      return false;
    }
    ++count;
  }
  return count == n && n > 0;
}

bool luaToJson(lua_State* L, int idx, JsonVariant dst, const int depth) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      dst.set(nullptr);
      return true;
    case LUA_TBOOLEAN:
      dst.set(lua_toboolean(L, idx) != 0);
      return true;
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) {
        dst.set(static_cast<long long>(lua_tointeger(L, idx)));
      } else {
        dst.set(static_cast<double>(lua_tonumber(L, idx)));
      }
      return true;
    case LUA_TSTRING: {
      size_t len = 0;
      const char* str = lua_tolstring(L, idx, &len);
      // Copia: el string de Lua puede morir antes de serializar.
      dst.set(std::string(str, len));
      return true;
    }
    case LUA_TTABLE:
      break;
    default:
      return false;  // funciones, userdata, hilos: no viajan
  }
  if (depth > LuaApp::ARGS_DEPTH) return false;
  if (!lua_checkstack(L, 4)) return false;
  if (isSequence(L, idx)) {
    JsonArray arr = dst.to<JsonArray>();
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_rawgeti(L, idx, i);
      JsonVariant slot = arr.add<JsonVariant>();
      const bool ok = luaToJson(L, -1, slot, depth + 1);
      lua_pop(L, 1);
      if (!ok) return false;
    }
    return true;
  }
  JsonObject obj = dst.to<JsonObject>();
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    // clave en -2, valor en -1
    std::string key;
    if (lua_type(L, -2) == LUA_TSTRING) {
      key = lua_tostring(L, -2);
    } else if (lua_type(L, -2) == LUA_TNUMBER) {
      key = std::to_string(static_cast<long long>(lua_tointeger(L, -2)));
    } else {
      lua_pop(L, 1);
      continue;
    }
    if (lua_type(L, -1) != LUA_TNIL) {
      JsonVariant slot = obj[key].to<JsonVariant>();
      if (!luaToJson(L, -1, slot, depth + 1)) {
        lua_pop(L, 2);
        return false;
      }
    }
    lua_pop(L, 1);
  }
  return true;
}

// --- JSON -> Lua (lo que contesta el servidor) ------------------------------
void jsonToLua(lua_State* L, JsonVariantConst v) {
  luaL_checkstack(L, 4, "respuesta demasiado anidada");
  if (v.isNull()) {
    lua_pushnil(L);
  } else if (v.is<bool>()) {
    lua_pushboolean(L, v.as<bool>() ? 1 : 0);
  } else if (v.is<long long>()) {
    lua_pushinteger(L, static_cast<lua_Integer>(v.as<long long>()));
  } else if (v.is<double>()) {
    lua_pushnumber(L, static_cast<lua_Number>(v.as<double>()));
  } else if (v.is<const char*>()) {
    lua_pushstring(L, v.as<const char*>());
  } else if (v.is<JsonArrayConst>()) {
    JsonArrayConst arr = v.as<JsonArrayConst>();
    lua_createtable(L, static_cast<int>(arr.size()), 0);
    lua_Integer i = 1;
    for (JsonVariantConst e : arr) {
      jsonToLua(L, e);
      lua_rawseti(L, -2, i++);
    }
  } else if (v.is<JsonObjectConst>()) {
    JsonObjectConst obj = v.as<JsonObjectConst>();
    lua_createtable(L, 0, static_cast<int>(obj.size()));
    for (JsonPairConst kv : obj) {
      jsonToLua(L, kv.value());
      lua_setfield(L, -2, kv.key().c_str());
    }
  } else {
    lua_pushnil(L);
  }
}

// cp.listen(seg [, pregunta]) -> true si quedó pedido
int cpListen(lua_State* L) {
  const int seconds = static_cast<int>(
      std::max<lua_Integer>(1, std::min<lua_Integer>(luaL_optinteger(L, 1, 10), LuaApp::LISTEN_MAX_S)));
  std::string question;
  if (!lua_isnoneornil(L, 2)) question = boundedText(L, 2);
  if (queueFull()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  // Dos escuchas encoladas no tienen sentido: el micrófono es uno y la segunda
  // pregunta pisaría a la primera en la pantalla.
  for (const LuaApp::Request& r : g_requests) {
    if (r.kind == LuaApp::Request::Kind::Listen) {
      lua_pushboolean(L, 0);
      return 1;
    }
  }
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::Listen;
  req.seconds = seconds;
  req.a = std::move(question);
  g_requests.push_back(std::move(req));
  lua_pushboolean(L, 1);
  return 1;
}

// cp.call(servicio [, args]) -> id, o nil si no entra
int cpCall(lua_State* L) {
  const char* service = luaL_checkstring(L, 1);
  if (!validToken(service)) {
    LOG_ERR(TAG, "%s: cp.call: servicio inválido", g_appStem.c_str());
    lua_pushnil(L);
    return 1;
  }
  if (queueFull()) {
    lua_pushnil(L);
    return 1;
  }
  std::string args = "{}";
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    JsonDocument doc(&PsramAllocator::instance());
    JsonVariant root = doc.to<JsonVariant>();
    if (!luaToJson(L, 2, root, 1) || doc.overflowed()) {
      LOG_ERR(TAG, "%s: cp.call: args no se pueden convertir (profundidad %d, memoria)", g_appStem.c_str(),
              LuaApp::ARGS_DEPTH);
      lua_pushnil(L);
      return 1;
    }
    // Una tabla vacía es un objeto vacío, no un array: `args` es un objeto.
    if (root.is<JsonArray>() && root.size() == 0) doc.to<JsonObject>();
    if (measureJson(doc) > LuaApp::ARGS_CAP) {
      LOG_ERR(TAG, "%s: cp.call: args supera %u KB", g_appStem.c_str(), (unsigned)(LuaApp::ARGS_CAP / 1024));
      lua_pushnil(L);
      return 1;
    }
    args.clear();
    serializeJson(doc, args);
  }
  const int id = ++g_nextId;
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::Call;
  req.id = id;
  req.a = service;
  req.b = std::move(args);
  g_requests.push_back(std::move(req));
  lua_pushinteger(L, id);
  return 1;
}

// cp.download(fileId, nombre [, destino]) -> id, o nil
int cpDownload(lua_State* L) {
  const char* fileId = luaL_checkstring(L, 1);
  const char* name = luaL_checkstring(L, 2);
  const char* dest = luaL_optstring(L, 3, "app");
  if (!validToken(fileId) || !LuaApp::validName(name) || (strcmp(dest, "app") != 0 && strcmp(dest, "books") != 0)) {
    LOG_ERR(TAG, "%s: cp.download: argumentos inválidos", g_appStem.c_str());
    lua_pushnil(L);
    return 1;
  }
  if (queueFull()) {
    lua_pushnil(L);
    return 1;
  }
  const int id = ++g_nextId;
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::Download;
  req.id = id;
  req.a = fileId;
  req.b = name;
  req.c = dest;
  g_requests.push_back(std::move(req));
  lua_pushinteger(L, id);
  return 1;
}

int cpBusy(lua_State* L) {
  lua_pushboolean(L, (g_hostBusy || !g_requests.empty()) ? 1 : 0);
  return 1;
}

// cp.files() -> { "a.txt", "b.epub", ... } de /Apps/data/<app>
int cpFiles(lua_State* L) {
  lua_newtable(L);
  if (!Storage.exists(g_dataDir.c_str())) return 1;
  lua_Integer i = 1;
  for (const String& entry : Storage.listFiles(g_dataDir.c_str(), 100)) {
    const std::string name = entry.c_str();
    // Un .tmp es una escritura a medio camino, no un archivo de la app.
    if (name.size() > 4 && name.compare(name.size() - 4, 4, TMP_SUFFIX) == 0) continue;
    if (!LuaApp::validName(name.c_str())) continue;
    lua_pushstring(L, name.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// cp.read(nombre [, desde, largo]) -> texto o nil. `desde` es 0-based.
int cpRead(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  if (!LuaApp::validName(name)) {
    lua_pushnil(L);
    return 1;
  }
  const std::string path = dataPath(name);
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t size = f.size();
  size_t from = 0;
  size_t len = size;
  if (!lua_isnoneornil(L, 2)) {
    const lua_Integer d = luaL_checkinteger(L, 2);
    from = d < 0 ? 0 : static_cast<size_t>(d);
    const lua_Integer l = luaL_optinteger(L, 3, static_cast<lua_Integer>(LuaApp::READ_CAP));
    len = l < 0 ? 0 : static_cast<size_t>(l);
  }
  if (from >= size) {
    f.close();
    lua_pushlstring(L, "", 0);
    return 1;
  }
  len = std::min(len, size - from);
  if (len > LuaApp::READ_CAP) {
    // Entero no entra: la app tiene que pedir por rango. Se dice en el log en
    // vez de devolver la mitad como si fuera todo.
    LOG_ERR(TAG, "%s: cp.read(%s): %u B no entran en %u KB; pedir con rango", g_appStem.c_str(), name, (unsigned)len,
            (unsigned)(LuaApp::READ_CAP / 1024));
    f.close();
    lua_pushnil(L);
    return 1;
  }
  char* buf = static_cast<char*>(bigAlloc(len ? len : 1));
  if (!buf) {
    f.close();
    lua_pushnil(L);
    return 1;
  }
  bool ok = f.seek(from);
  const int got = ok && len ? f.read(buf, len) : 0;
  f.close();
  if (!ok || got < 0) {
    heap_caps_free(buf);
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf, static_cast<size_t>(got));
  heap_caps_free(buf);
  return 1;
}

// cp.write(nombre, texto) -> true/false. Atómico: se escribe a `.tmp` y se
// renombra, así un corte a la mitad deja el archivo viejo entero y no uno a
// medias (la misma regla que `SDCardManager::writeFile`, que acá no se usa
// porque toma una String de heap interno y 64 KB ahí no caben).
int cpWrite(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  if (!LuaApp::validName(name) || len > LuaApp::WRITE_CAP) {
    if (len > LuaApp::WRITE_CAP) {
      LOG_ERR(TAG, "%s: cp.write(%s): %u B supera el tope de %u KB", g_appStem.c_str(), name, (unsigned)len,
              (unsigned)(LuaApp::WRITE_CAP / 1024));
    }
    lua_pushboolean(L, 0);
    return 1;
  }
  Storage.ensureDirectoryExists(g_dataDir.c_str());
  const std::string path = dataPath(name);
  const std::string tmp = path + TMP_SUFFIX;
  if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
  HalFile f;
  if (!Storage.openFileForWrite(TAG, tmp, f)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  const size_t written = len ? f.write(reinterpret_cast<const uint8_t*>(data), len) : 0;
  f.close();
  if (written != len) {
    Storage.remove(tmp.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  lua_pushboolean(L, Storage.rename(tmp.c_str(), path.c_str()) ? 1 : 0);
  return 1;
}

int cpRemove(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  if (!LuaApp::validName(name)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  const std::string path = dataPath(name);
  lua_pushboolean(L, Storage.exists(path.c_str()) && Storage.remove(path.c_str()) ? 1 : 0);
  return 1;
}

int cpSize(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  if (!LuaApp::validName(name)) {
    lua_pushnil(L);
    return 1;
  }
  HalFile f;
  if (!Storage.openFileForRead(TAG, dataPath(name), f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t size = f.size();
  f.close();
  lua_pushinteger(L, static_cast<lua_Integer>(size));
  return 1;
}

// cp.view(nombre [, titulo]) -> true si existía. Abrirlo lo hace el host.
int cpView(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  std::string title;
  if (!lua_isnoneornil(L, 2)) title = boundedText(L, 2);
  if (!LuaApp::validName(name) || !Storage.exists(dataPath(name).c_str()) || queueFull()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::View;
  req.a = name;
  req.b = title.empty() ? std::string(name) : title;
  g_requests.push_back(std::move(req));
  lua_pushboolean(L, 1);
  return 1;
}

// cp.open_book(nombre) -> true si existía en /Books/<app>/. La app se cierra.
int cpOpenBook(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  const std::string path = g_booksDir + "/" + name;
  if (!LuaApp::validName(name) || !Storage.exists(path.c_str()) || queueFull()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::OpenBook;
  req.a = name;
  g_requests.push_back(std::move(req));
  lua_pushboolean(L, 1);
  return 1;
}

// cp.say(texto) -> id, o nil. El host pide la voz al servidor
// (`GET /api/tts`) y la reproduce por el parlante mientras la app sigue; llega
// `on_reply(id, true, {})` cuando el audio arrancó, o `ok=false` con `error`
// ("sin voz (N)", "cancelado", "sin vincular"). El texto va acotado a
// TEXT_CAP (512 bytes): un párrafo, no un capítulo; para eso está el visor.
int cpSay(lua_State* L) {
  const char* text = boundedText(L, 1);
  if (*text == '\0' || queueFull()) {
    lua_pushnil(L);
    return 1;
  }
  const int id = ++g_nextId;
  LuaApp::Request req;
  req.kind = LuaApp::Request::Kind::Say;
  req.id = id;
  req.a = text;
  g_requests.push_back(std::move(req));
  lua_pushinteger(L, id);
  return 1;
}

const luaL_Reg CP_API[] = {
    {"clear", cpClear},        {"text", cpText},     {"textw", cpTextWidth}, {"texth", cpTextHeight},
    {"rect", cpRect},          {"line", cpLine},     {"image", cpImage},     {"selection", cpSelection},
    {"width", cpWidth},        {"height", cpHeight}, {"motion", cpMotion},   {"ms", cpMs},
    {"beep", cpBeep},          {"log", cpLog},       {"quit", cpQuit},       {"save", cpSave},
    {"load", cpLoad},          {"time", cpTime},     {"listen", cpListen},   {"call", cpCall},
    {"download", cpDownload},  {"busy", cpBusy},     {"files", cpFiles},     {"read", cpRead},
    {"write", cpWrite},        {"remove", cpRemove}, {"size", cpSize},       {"view", cpView},
    {"open_book", cpOpenBook}, {"say", cpSay},       {nullptr, nullptr},
};

// --- Trabajo dentro del worker ------------------------------------------

struct LoadJob {
  lua_State* L = nullptr;
  const char* source = nullptr;
  size_t sourceSize = 0;
  std::string chunkName;
  bool ok = false;
  std::string error;
};

void loadEntry(void* p) {
  auto* job = static_cast<LoadJob*>(p);
  lua_State* L = job->L;
  if (luaL_loadbuffer(L, job->source, job->sourceSize, job->chunkName.c_str()) != LUA_OK) {
    job->error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "no compila";
    lua_pop(L, 1);
    return;
  }
  luasandbox::armStepLimit(L, LuaApp::STEP_LIMIT);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    job->error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "falló al arrancar";
    lua_pop(L, 1);
    luasandbox::clearStepLimit(L);
    return;
  }
  luasandbox::clearStepLimit(L);
  job->ok = true;
}

struct CallJob {
  lua_State* L = nullptr;
  const char* fn = nullptr;
  const char* arg = nullptr;
  // Empujador alternativo: arma los argumentos ADENTRO del worker (la tabla de
  // una respuesta se construye ahí, con el stack del worker) y devuelve cuántos
  // puso.
  int (*push)(lua_State*, void*) = nullptr;
  void* ctx = nullptr;
  bool repaint = false;
  bool missing = false;
  std::string error;
};

void callEntry(void* p) {
  auto* job = static_cast<CallJob*>(p);
  lua_State* L = job->L;
  if (lua_getglobal(L, job->fn) != LUA_TFUNCTION) {
    lua_pop(L, 1);
    job->missing = true;
    return;
  }
  int argc = 0;
  if (job->push) {
    argc = job->push(L, job->ctx);
  } else if (job->arg) {
    lua_pushstring(L, job->arg);
    argc = 1;
  }
  luasandbox::armStepLimit(L, LuaApp::STEP_LIMIT);
  const int rc = lua_pcall(L, argc, 1, 0);
  luasandbox::clearStepLimit(L);
  if (rc != LUA_OK) {
    job->error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
    lua_pop(L, 1);
    return;
  }
  job->repaint = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
}
}  // namespace

// --- Catálogo ------------------------------------------------------------

const char* LuaApp::dir() { return APPS_DIR; }

namespace {
// Las primeras líneas de un archivo, para leerle el comentario de apertura.
std::string readHead(const std::string& path, const size_t max = 512) {
  HalFile f;
  if (!Storage.openFileForRead("LUA", path, f)) return "";
  std::string head;
  head.resize(max);
  const int n = f.read(reinterpret_cast<uint8_t*>(&head[0]), max);
  f.close();
  head.resize(n > 0 ? static_cast<size_t>(n) : 0);
  return head;
}

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '.')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}
}  // namespace

// `-- Reloj: la hora grande, la fecha debajo.` → título "Reloj", descripción
// "la hora grande, la fecha debajo". Vale `:` o `.` como separador, y sólo si
// lo que queda antes es corto (hasta 32 caracteres): si no, el archivo no
// tiene título y se usa el nombre con mayúscula. La descripción es el resto de
// esa primera línea de comentario, nada más: la lista muestra un renglón.
void LuaApp::titleFromHeader(const std::string& header, const std::string& stem, std::string& title,
                             std::string& description) {
  title = stem;
  if (!title.empty() && title[0] >= 'a' && title[0] <= 'z') title[0] = static_cast<char>(title[0] - 'a' + 'A');
  description.clear();
  size_t start = 0;
  while (start < header.size() && (header[start] == ' ' || header[start] == '\n' || header[start] == '\r')) ++start;
  if (header.compare(start, 2, "--") != 0) return;
  size_t end = header.find('\n', start);
  std::string line = header.substr(start + 2, end == std::string::npos ? std::string::npos : end - start - 2);
  line = trimmed(line);
  if (line.empty()) return;
  const size_t sep = line.find_first_of(":.");
  if (sep == std::string::npos || sep == 0 || sep > 32) return;
  title = trimmed(line.substr(0, sep));
  description = trimmed(line.substr(sep + 1));
  if (!description.empty() && description[0] >= 'a' && description[0] <= 'z') {
    description[0] = static_cast<char>(description[0] - 'a' + 'A');
  }
}

std::vector<LuaApp::Entry> LuaApp::installed() {
  std::vector<Entry> out;
  if (!Storage.exists(APPS_DIR)) return out;
  for (const String& entry : Storage.listFiles(APPS_DIR, 60)) {
    const std::string name = entry.c_str();
    if (name.size() <= strlen(EXT)) continue;
    if (name.compare(name.size() - strlen(EXT), strlen(EXT), EXT) != 0) continue;
    Entry e;
    e.path = std::string(APPS_DIR) + "/" + name;
    e.name = name.substr(0, name.size() - strlen(EXT));
    titleFromHeader(readHead(e.path), e.name, e.title, e.description);
    out.push_back(e);
  }
  return out;
}

// --- Ciclo de vida -------------------------------------------------------

LuaApp::~LuaApp() { close(); }

bool LuaApp::open(GfxRenderer& renderer, const std::string& path) {
  VmGuard guard;
  close();
  path_ = path;
  const size_t slash = path.find_last_of('/');
  name_ = slash == std::string::npos ? path : path.substr(slash + 1);
  if (name_.size() > strlen(EXT)) name_ = name_.substr(0, name_.size() - strlen(EXT));
  {
    std::string description;
    titleFromHeader(readHead(path), name_, title_, description);
  }
  g_appStem = name_;
  // La carpeta de la app sale del nombre saneado: lo que no es [A-Za-z0-9._-]
  // pasa a `_`, y un nombre que empiece con punto no puede hacer carpeta oculta.
  dirName_.clear();
  for (const char c : name_) dirName_ += nameCharOk(c) ? c : '_';
  if (dirName_.size() > 32) dirName_.resize(32);
  if (dirName_.empty() || dirName_[0] == '.') dirName_ = "app" + dirName_;
  g_dataDir = std::string(DATA_ROOT) + "/" + dirName_;
  g_booksDir = std::string(BOOKS_ROOT) + "/" + dirName_;
  g_requests.clear();
  g_hostBusy = false;
  g_renderer = &renderer;
  g_quit = false;
  quit_ = false;

  HalFile script = Storage.open(path.c_str());
  if (!script || script.isDirectory() || script.size() > SCRIPT_CAP) {
    error_ = "la app supera el tamaño permitido";
    return false;
  }
  // El script se lee ENTERO y a PSRAM. Hasta 1.5.113 iba por
  // `Storage.readFile()`, que recorta EN SILENCIO a un cuarto del bloque de heap
  // libre (~28 KB con un heap interno de ~110 KB): toda app de más de eso
  // llegaba cortada y Lua reventaba en el último renglón leído (viajes 763/765,
  // sudoku 768, libros 812: los tres al mismo byte, ~28 KB). El tope de una app
  // es SCRIPT_CAP y se lee completo o no se abre.
  const size_t scriptSize = static_cast<size_t>(script.size());
  char* buffer = static_cast<char*>(heap_caps_malloc(scriptSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<char*>(malloc(scriptSize + 1));
  if (!buffer) {
    script.close();
    error_ = "no hay memoria para leer la app";
    return false;
  }
  size_t got = 0;
  while (got < scriptSize) {
    const int r = script.read(reinterpret_cast<uint8_t*>(buffer) + got, scriptSize - got);
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  script.close();
  buffer[got] = '\0';
  if (got == 0 || got != scriptSize) {
    LOG_ERR(TAG, "%s: se leyeron %u de %u B del script", name_.c_str(), (unsigned)got, (unsigned)scriptSize);
    free(buffer);
    error_ = got == 0 ? "el archivo está vacío o no se pudo leer" : "no se pudo leer la app entera";
    return false;
  }
  struct SourceGuard {
    char* p;
    ~SourceGuard() { free(p); }
  } sourceGuard{buffer};
  struct {
    const char* c_str() const { return p; }
    size_t length() const { return n; }
    const char* p;
    size_t n;
  } source{buffer, got};

  state_ = luasandbox::create(MEM_CAP);
  if (!state_) {
    error_ = "no hay memoria para el intérprete";
    return false;
  }

  luaL_newlib(state_, CP_API);
  lua_setglobal(state_, "cp");
  // print va al log del aparato, que es lo que se lee en /board/log.
  lua_getglobal(state_, "cp");
  lua_getfield(state_, -1, "log");
  lua_setglobal(state_, "print");
  lua_pop(state_, 1);

  LoadJob job;
  job.L = state_;
  job.source = source.c_str();
  job.sourceSize = source.length();
  job.chunkName = "@" + name_;
  uint32_t used = 0;
  if (!tasks::runBounded("lua-loader", STACK, loadEntry, &job, &used)) {
    error_ = "no hay memoria para el worker";
    close();
    return false;
  }
  if (!job.ok) {
    error_ = job.error;
    close();
    return false;
  }
  LOG_INF(TAG, "%s abierta (%u B de script, %u B de stack)", name_.c_str(), (unsigned)source.length(), (unsigned)used);

  callback("on_open", nullptr);
  // on_tick cuesta un worker cada TICK_MS: si la app no lo define, no se llama.
  lua_getglobal(state_, "on_tick");
  hasTick_ = lua_isfunction(state_, -1);
  lua_pop(state_, 1);
  return error_.empty();
}

void LuaApp::close() {
  VmGuard guard;
  luasandbox::destroy(state_);
  state_ = nullptr;
  g_renderer = nullptr;
  g_requests.clear();
  g_hostBusy = false;
  hasTick_ = false;
}

bool LuaApp::callback(const char* fn, const char* arg) { return callbackWith(fn, nullptr, const_cast<char*>(arg)); }

bool LuaApp::callbackWith(const char* fn, int (*push)(lua_State*, void*), void* ctx) {
  VmGuard guard;
  if (!state_ || !error_.empty()) return false;
  CallJob job;
  job.L = state_;
  job.fn = fn;
  job.push = push;
  job.ctx = ctx;
  if (!push) job.arg = static_cast<const char*>(ctx);
  if (!tasks::runBounded("lua-app", STACK, callEntry, &job)) {
    error_ = "no hay memoria para el worker";
    return false;
  }
  if (!job.error.empty()) {
    error_ = job.error;
    LOG_ERR(TAG, "%s: %s", name_.c_str(), error_.c_str());
    return false;
  }
  if (g_quit) quit_ = true;
  return job.repaint;
}

bool LuaApp::onKey(const char* key) { return callback("on_key", key); }

bool LuaApp::onTick() { return hasTick_ ? callback("on_tick", nullptr) : false; }

void LuaApp::onDraw() { callback("on_draw", nullptr); }

// --- Las puertas: lo que el host atiende ---------------------------------------

bool LuaApp::validName(const char* name) {
  if (!name) return false;
  const size_t len = strlen(name);
  if (len == 0 || len > NAME_CAP || name[0] == '.') return false;
  for (size_t i = 0; i < len; ++i) {
    if (!nameCharOk(name[i])) return false;
  }
  return true;
}

std::string LuaApp::dataDir() const { return std::string(DATA_ROOT) + "/" + dirName_; }
std::string LuaApp::booksDir() const { return std::string(BOOKS_ROOT) + "/" + dirName_; }

bool LuaApp::takeRequest(Request& out) {
  VmGuard guard;  // g_requests la llena el worker de un callback y la vacía el loop
  if (g_requests.empty()) return false;
  out = std::move(g_requests.front());
  g_requests.pop_front();
  return true;
}

bool LuaApp::hasRequests() const {
  VmGuard guard;
  return !g_requests.empty();
}

void LuaApp::setBusy(const bool busy) {
  VmGuard guard;
  g_hostBusy = busy;
}

bool LuaApp::cancelQueued() {
  VmGuard guard;
  bool repaint = false;
  while (!g_requests.empty()) {
    Request r = std::move(g_requests.front());
    g_requests.pop_front();
    switch (r.kind) {
      case Request::Kind::Listen:
        repaint |= onHeard(nullptr);
        break;
      case Request::Kind::Call:
      case Request::Kind::Download:
      case Request::Kind::Say:
        repaint |= onReplyError(r.id, "cancelado");
        break;
      case Request::Kind::View:
      case Request::Kind::OpenBook:
        break;  // no tienen respuesta
    }
    if (!ok()) break;
  }
  return repaint;
}

bool LuaApp::onHeard(const char* textOrNull) {
  VmGuard guard;
  if (!state_) return false;
  // Sin on_heard no es error: la app pidió escuchar y no le interesa el texto.
  lua_getglobal(state_, "on_heard");
  const bool has = lua_isfunction(state_, -1);
  lua_pop(state_, 1);
  if (!has) return false;
  struct Ctx {
    const char* text;
  } ctx{textOrNull};
  return callbackWith(
      "on_heard",
      [](lua_State* L, void* p) {
        const auto* c = static_cast<Ctx*>(p);
        if (c->text) {
          lua_pushstring(L, c->text);
        } else {
          lua_pushnil(L);
        }
        return 1;
      },
      &ctx);
}

namespace {
struct ReplyCtx {
  int id;
  bool ok;
  const std::string* json;  // cuerpo del servidor, o nullptr
  const char* error;        // texto de error, o nullptr
  size_t bytes;             // para {bytes=n}
  bool bytesReply;
};

// Arma (id, ok, tabla) adentro del worker. Si `json` no parsea, ok pasa a
// false y la tabla dice por qué: la app siempre recibe una tabla, nunca nil.
int pushReply(lua_State* L, void* p) {
  auto* c = static_cast<ReplyCtx*>(p);
  lua_pushinteger(L, c->id);
  if (c->json) {
    JsonDocument doc(&PsramAllocator::instance());
    const DeserializationError err = deserializeJson(doc, *c->json);
    if (err != DeserializationError::Ok) {
      lua_pushboolean(L, 0);
      lua_createtable(L, 0, 1);
      lua_pushstring(L, "respuesta ilegible del servidor");
      lua_setfield(L, -2, "error");
      return 3;
    }
    const bool ok = doc["ok"] | false;
    lua_pushboolean(L, ok ? 1 : 0);
    if (doc.is<JsonObjectConst>()) {
      jsonToLua(L, doc.as<JsonVariantConst>());
    } else {
      // Un JSON que no es objeto (un array, un número) no es una respuesta del
      // contrato; se envuelve para que `tabla.error` exista.
      lua_createtable(L, 0, 1);
      lua_pushstring(L, "respuesta ilegible del servidor");
      lua_setfield(L, -2, "error");
    }
    return 3;
  }
  lua_pushboolean(L, c->ok ? 1 : 0);
  lua_createtable(L, 0, 1);
  if (c->bytesReply) {
    lua_pushinteger(L, static_cast<lua_Integer>(c->bytes));
    lua_setfield(L, -2, "bytes");
  } else {
    lua_pushstring(L, c->error ? c->error : "error");
    lua_setfield(L, -2, "error");
  }
  return 3;
}
}  // namespace

bool LuaApp::onReply(const int id, const std::string& json) {
  VmGuard guard;
  if (!state_) return false;
  lua_getglobal(state_, "on_reply");
  const bool has = lua_isfunction(state_, -1);
  lua_pop(state_, 1);
  if (!has) return false;
  ReplyCtx ctx{id, true, &json, nullptr, 0, false};
  return callbackWith("on_reply", pushReply, &ctx);
}

bool LuaApp::onReplyError(const int id, const char* error) {
  VmGuard guard;
  if (!state_) return false;
  lua_getglobal(state_, "on_reply");
  const bool has = lua_isfunction(state_, -1);
  lua_pop(state_, 1);
  if (!has) return false;
  ReplyCtx ctx{id, false, nullptr, error, 0, false};
  return callbackWith("on_reply", pushReply, &ctx);
}

bool LuaApp::onReplyBytes(const int id, const size_t bytes) {
  VmGuard guard;
  if (!state_) return false;
  lua_getglobal(state_, "on_reply");
  const bool has = lua_isfunction(state_, -1);
  lua_pop(state_, 1);
  if (!has) return false;
  ReplyCtx ctx{id, true, nullptr, nullptr, bytes, true};
  return callbackWith("on_reply", pushReply, &ctx);
}
