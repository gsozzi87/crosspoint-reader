#include "LuaApp.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "../HubStore.h"
#include "../TaskConfig.h"
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

// --- La tabla cp ---------------------------------------------------------

int clampCoord(const lua_Integer v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return static_cast<int>(v);
}

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
  const int x = clampCoord(luaL_checkinteger(L, 1));
  const int y = clampCoord(luaL_checkinteger(L, 2));
  const char* text = luaL_checkstring(L, 3);
  const int font = fontFor(luaL_optinteger(L, 4, 12));
  const bool bold = lua_toboolean(L, 5) != 0;
  if (g_renderer) {
    g_renderer->drawText(font, x, y, text, true, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  return 0;
}

int cpTextWidth(lua_State* L) {
  const char* text = luaL_checkstring(L, 1);
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
  const int x = clampCoord(luaL_checkinteger(L, 1));
  const int y = clampCoord(luaL_checkinteger(L, 2));
  const int w = clampCoord(luaL_checkinteger(L, 3));
  const int h = clampCoord(luaL_checkinteger(L, 4));
  const bool filled = lua_toboolean(L, 5) != 0;
  if (!g_renderer) return 0;
  if (filled) {
    g_renderer->fillRect(x, y, w, h, true);
  } else {
    g_renderer->drawRect(x, y, w, h, static_cast<int>(luaL_optinteger(L, 6, 1)), true);
  }
  return 0;
}

int cpLine(lua_State* L) {
  if (!g_renderer) return 0;
  g_renderer->drawLine(clampCoord(luaL_checkinteger(L, 1)), clampCoord(luaL_checkinteger(L, 2)),
                       clampCoord(luaL_checkinteger(L, 3)), clampCoord(luaL_checkinteger(L, 4)),
                       static_cast<int>(luaL_optinteger(L, 5, 1)), true);
  return 0;
}

// El resalte del sistema visual (docs/ws397/DISENO.md): pestaña, marco y
// franjas SÓLO en los márgenes. Se expone para que una app con una lista se vea
// como el resto del aparato en vez de inventar su propio negro macizo.
int cpSelection(lua_State* L) {
  if (!g_renderer) return 0;
  drawSelectionRow(*g_renderer, clampCoord(luaL_checkinteger(L, 1)), clampCoord(luaL_checkinteger(L, 2)),
                   clampCoord(luaL_checkinteger(L, 3)), clampCoord(luaL_checkinteger(L, 4)), 0);
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
  if (strcmp(which, "ok") == 0) s = uisound::Sound::Select;
  else if (strcmp(which, "back") == 0) s = uisound::Sound::Back;
  else if (strcmp(which, "error") == 0) s = uisound::Sound::Error;
  UI_SOUND.play(s);
  return 0;
}

int cpLog(lua_State* L) {
  const int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i) {
    if (i > 1) line += ' ';
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    line.append(s, len);
    lua_pop(L, 1);
  }
  LOG_INF(TAG, "%s: %s", g_appStem.c_str(), line.c_str());
  return 0;
}

int cpQuit(lua_State*) {
  g_quit = true;
  return 0;
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
  const size_t nul = std::string(data, len).find('\0');
  if (nul != std::string::npos) len = nul;
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

const luaL_Reg CP_API[] = {
    {"clear", cpClear},        {"text", cpText},      {"textw", cpTextWidth}, {"texth", cpTextHeight},
    {"rect", cpRect},          {"line", cpLine},      {"selection", cpSelection},
    {"width", cpWidth},        {"height", cpHeight},  {"motion", cpMotion},   {"ms", cpMs},
    {"beep", cpBeep},          {"log", cpLog},        {"quit", cpQuit},       {"save", cpSave},
    {"load", cpLoad},          {nullptr, nullptr},
};

// --- Trabajo dentro del worker ------------------------------------------

struct LoadJob {
  lua_State* L = nullptr;
  std::string source;
  std::string chunkName;
  bool ok = false;
  std::string error;
};

void loadEntry(void* p) {
  auto* job = static_cast<LoadJob*>(p);
  lua_State* L = job->L;
  if (luaL_loadbuffer(L, job->source.data(), job->source.size(), job->chunkName.c_str()) != LUA_OK) {
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
  if (job->arg) {
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
    out.push_back(e);
  }
  return out;
}

// --- Ciclo de vida -------------------------------------------------------

LuaApp::~LuaApp() { close(); }

bool LuaApp::open(GfxRenderer& renderer, const std::string& path) {
  close();
  path_ = path;
  const size_t slash = path.find_last_of('/');
  name_ = slash == std::string::npos ? path : path.substr(slash + 1);
  if (name_.size() > strlen(EXT)) name_ = name_.substr(0, name_.size() - strlen(EXT));
  g_appStem = name_;
  g_renderer = &renderer;
  g_quit = false;
  quit_ = false;

  const String source = Storage.readFile(path.c_str());
  if (source.length() == 0) {
    error_ = "el archivo está vacío o no se pudo leer";
    return false;
  }

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
  luasandbox::destroy(state_);
  state_ = nullptr;
  g_renderer = nullptr;
  hasTick_ = false;
}

bool LuaApp::callback(const char* fn, const char* arg) {
  if (!state_ || !error_.empty()) return false;
  CallJob job;
  job.L = state_;
  job.fn = fn;
  job.arg = arg;
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
