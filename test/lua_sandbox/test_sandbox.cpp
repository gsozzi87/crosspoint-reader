// Prueba de escritorio del cajón donde corren las apps de la tarjeta.
//
// Es la única forma de estar seguro de que `io`, `os`, `require` y `load` NO
// están cuando uno cree que no están: en el aparato eso se vería recién el día
// que una app maliciosa (o distraída) intente abrir un archivo.
//
// Compilar y correr:
//   g++ -std=c++17 -I lib/Lua/src -I src/lua test/lua_sandbox/test_sandbox.cpp \
//       src/lua/LuaSandbox.cpp lib/Lua/src/*.c -o /tmp/test_sandbox && /tmp/test_sandbox

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "LuaSandbox.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {
int failures = 0;

void check(const bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FALLA", what);
  if (!ok) ++failures;
}

// Corre un pedazo de Lua en un cajón nuevo. Devuelve "" si salió bien, o el
// mensaje de error. `steps` en 0 = sin guardia de instrucciones.
std::string run(const char* code, const int steps = 0, const size_t cap = 1024 * 1024) {
  lua_State* L = luasandbox::create(cap);
  if (!L) return "sin memoria";
  std::string err;
  if (luaL_loadbuffer(L, code, strlen(code), "=prueba") != LUA_OK) {
    err = lua_tostring(L, -1);
  } else {
    if (steps > 0) luasandbox::armStepLimit(L, steps);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) err = lua_tostring(L, -1);
    luasandbox::clearStepLimit(L);
  }
  luasandbox::destroy(L);
  return err;
}

bool ranClean(const char* code) { return run(code).empty(); }
bool failed(const char* code) { return !run(code).empty(); }

// --- Las apps de ejemplo -------------------------------------------------
//
// El `cp` de acá es un doble: devuelve valores plausibles y no dibuja nada. Lo
// que se prueba es que las apps de examples/Apps compilen y que sus callbacks
// corran sin error contra la API que promete docs/ws397/APPS_LUA.md. Si alguien
// agrega una función a `cp` en el aparato y no acá, una app que la use falla
// en esta prueba, que es justamente cuando conviene enterarse.

int stubZero(lua_State* L) { lua_pushinteger(L, 0); return 1; }
int stubNil(lua_State* L) { lua_pushnil(L); return 1; }
int stubNone(lua_State*) { return 0; }
int stubWidth(lua_State* L) { lua_pushinteger(L, 480); return 1; }
int stubHeight(lua_State* L) { lua_pushinteger(L, 800); return 1; }
int stubTextW(lua_State* L) { lua_pushinteger(L, 8 * static_cast<int>(strlen(luaL_checkstring(L, 1)))); return 1; }
int stubTextH(lua_State* L) { lua_pushinteger(L, 18); return 1; }
int stubMs(lua_State* L) { lua_pushinteger(L, 12345); return 1; }
int stubTrue(lua_State* L) { lua_pushboolean(L, 1); return 1; }

// Con el reloj puesto y sin el reloj puesto: el aparato de verdad devuelve nil
// mientras no esté en hora, y una app que no lo contemple se rompe justo cuando
// alguien la abre recién sacada de la caja.
bool g_relojEnHora = true;

int stubTime(lua_State* L) {
  if (!g_relojEnHora) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 8);
  const auto campo = [L](const char* nombre, const lua_Integer valor) {
    lua_pushinteger(L, valor);
    lua_setfield(L, -2, nombre);
  };
  campo("year", 2026);
  campo("month", 9);
  campo("day", 14);
  campo("hour", 21);
  campo("min", 7);
  campo("sec", 42);
  campo("wday", 1);
  campo("epoch", 1789500462);
  return 1;
}

void installStubCp(lua_State* L) {
  static const luaL_Reg CP[] = {
      {"clear", stubNone},   {"text", stubNone},      {"textw", stubTextW}, {"texth", stubTextH},
      {"rect", stubNone},    {"line", stubNone},      {"selection", stubNone},
      {"width", stubWidth},  {"height", stubHeight},  {"motion", stubNil},  {"ms", stubMs},
      {"beep", stubNone},    {"log", stubNone},       {"quit", stubNone},   {"save", stubTrue},
      {"load", stubNil},     {"time", stubTime},      {nullptr, nullptr},
  };
  luaL_newlib(L, CP);
  lua_setglobal(L, "cp");
  (void)stubZero;
}

std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// Carga la app y le pega a todos los callbacks, como hace LuaAppsActivity.
std::string runApp(const char* path) {
  const std::string source = slurp(path);
  if (source.empty()) return "no se pudo leer el archivo";
  lua_State* L = luasandbox::create(1024 * 1024);
  if (!L) return "sin memoria";
  installStubCp(L);
  std::string err;
  if (luaL_loadbuffer(L, source.data(), source.size(), path) != LUA_OK) {
    err = lua_tostring(L, -1);
  } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = lua_tostring(L, -1);
  } else {
    struct Call { const char* fn; const char* arg; };
    const Call calls[] = {{"on_open", nullptr}, {"on_key", "up"},   {"on_key", "down"},
                          {"on_key", "ok"},     {"on_key", "back"}, {"on_tick", nullptr},
                          {"on_draw", nullptr}};
    for (const Call& c : calls) {
      if (lua_getglobal(L, c.fn) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        continue;
      }
      int argc = 0;
      if (c.arg) { lua_pushstring(L, c.arg); argc = 1; }
      luasandbox::armStepLimit(L, 400000);
      const int rc = lua_pcall(L, argc, 1, 0);
      luasandbox::clearStepLimit(L);
      if (rc != LUA_OK) {
        err = std::string(c.fn) + ": " + lua_tostring(L, -1);
        lua_pop(L, 1);
        break;
      }
      lua_pop(L, 1);
    }
  }
  luasandbox::destroy(L);
  return err;
}

// Una partida entera, no un toque a cada callback. Es lo que encuentra los
// errores de verdad: un índice fuera de rango cuando el tablero se llena, un
// bucle que no sale cuando no queda casilla libre, un estado final que nadie
// contempló. Se dibuja después de cada tecla, como hace el aparato.
std::string playApp(const char* path, const char* const* teclas, const int cuantas, const int vueltas) {
  const std::string source = slurp(path);
  if (source.empty()) return "no se pudo leer el archivo";
  lua_State* L = luasandbox::create(1024 * 1024);
  if (!L) return "sin memoria";
  installStubCp(L);
  std::string err;
  const auto llamar = [&](const char* fn, const char* arg) {
    if (!err.empty()) return;
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) {
      lua_pop(L, 1);
      return;
    }
    int argc = 0;
    if (arg) { lua_pushstring(L, arg); argc = 1; }
    luasandbox::armStepLimit(L, 400000);
    const int rc = lua_pcall(L, argc, 1, 0);
    luasandbox::clearStepLimit(L);
    if (rc != LUA_OK) {
      err = std::string(fn) + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_pop(L, 1);
  };
  if (luaL_loadbuffer(L, source.data(), source.size(), path) != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "no carga";
  } else {
    llamar("on_open", nullptr);
    for (int v = 0; v < vueltas && err.empty(); ++v) {
      for (int i = 0; i < cuantas && err.empty(); ++i) {
        llamar("on_key", teclas[i]);
        llamar("on_draw", nullptr);
      }
      llamar("on_tick", nullptr);
    }
  }
  luasandbox::destroy(L);
  return err;
}
}  // namespace

int main() {
  printf("-- lo que TIENE que estar --\n");
  check(ranClean("assert(math.floor(3.7) == 3)"), "math");
  check(ranClean("assert(('abc'):upper() == 'ABC')"), "string");
  check(ranClean("local t = {3,1,2}; table.sort(t); assert(t[1] == 1)"), "table");
  check(ranClean("assert(utf8.len('ñandú') == 5)"), "utf8");
  check(ranClean("local c = coroutine.create(function() coroutine.yield(1) end); coroutine.resume(c)"), "coroutine");
  check(ranClean("assert(select('#', 1, 2) == 2)"), "base: select");
  check(ranClean("assert(tostring(1) == '1' and tonumber('2') == 2)"), "base: tostring/tonumber");
  check(ranClean("assert(pcall(function() end))"), "base: pcall");

  printf("\n-- lo que NO tiene que estar --\n");
  check(failed("io.open('/etc/passwd')"), "io no existe");
  check(failed("os.execute('ls')"), "os no existe");
  check(failed("os.remove('/algo')"), "os.remove no existe");
  check(failed("require('socket')"), "require no existe");
  check(failed("load('return 1')()"), "load no existe");
  check(failed("loadstring('return 1')()"), "loadstring no existe");
  check(failed("dofile('/algo.lua')"), "dofile no existe");
  check(failed("loadfile('/algo.lua')"), "loadfile no existe");
  check(failed("string.dump(function() end)"), "string.dump no existe");
  check(failed("debug.getinfo(1)"), "debug no existe");
  check(failed("package.path = '/'"), "package no existe");

  printf("\n-- la guardia de instrucciones --\n");
  // Sin guardia esto no volvería nunca; con guardia tiene que dar error.
  const std::string spin = run("while true do end", 50000);
  check(!spin.empty(), "un bucle infinito termina en error y no cuelga");
  check(spin.find("demasiado") != std::string::npos, "y el error dice qué pasó");
  // La guardia NO puede cortar trabajo legítimo y corto.
  check(run("local s = 0; for i = 1, 1000 do s = s + i end; assert(s == 500500)", 400000).empty(),
        "un bucle normal pasa sin que la guardia lo toque");
  // Recursión sin fondo: tiene que dar error de Lua, no desbordar el stack de C.
  check(failed("local function f() return f() + 1 end; f()"), "una recursión sin fondo da error de Lua");

  printf("\n-- el tope de memoria --\n");
  const std::string fat = run("local t = {}; for i = 1, 1e7 do t[i] = i end", 0, 128 * 1024);
  check(!fat.empty(), "una tabla que crece sin parar se topa contra el techo");
  check(fat.find("memory") != std::string::npos, "y el error lo dice");
  check(luasandbox::memUsed() == 0, "al cerrar no queda nada pedido");

  printf("\n-- las apps de ejemplo y las de fábrica --\n");
  const char* APPS[] = {"examples/Apps/contador.lua", "examples/Apps/dados.lua",
                        "examples/Apps/reloj.lua", "examples/Apps/ahorcado.lua",
                        "examples/Apps/tresenraya.lua"};
  for (const char* file : APPS) {
    const std::string err = runApp(file);
    check(err.empty(), (std::string(file) + (err.empty() ? "" : ": " + err)).c_str());
  }

  printf("\n-- las mismas apps con el aparato SIN hora --\n");
  g_relojEnHora = false;
  for (const char* file : APPS) {
    const std::string err = runApp(file);
    check(err.empty(), (std::string(file) + " sin reloj" + (err.empty() ? "" : ": " + err)).c_str());
  }
  g_relojEnHora = true;

  printf("\n-- partidas enteras --\n");
  {
    // Ahorcado: recorrer el abecedario y probar cada letra. Con 26 letras se
    // termina la palabra o se pierde muchas veces; la app tiene que aguantar
    // las dos cosas y empezar de nuevo con OK.
    const char* teclas[] = {"down", "ok"};
    const std::string err = playApp("examples/Apps/ahorcado.lua", teclas, 2, 60);
    check(err.empty(), (std::string("ahorcado: 60 jugadas") + (err.empty() ? "" : ": " + err)).c_str());
  }
  {
    // Tres en raya: mover y poner hasta llenar el tablero varias veces. Acá es
    // donde un cursor que no sabe qué hacer sin casillas libres se cuelga.
    const char* teclas[] = {"down", "ok", "up", "ok"};
    const std::string err = playApp("examples/Apps/tresenraya.lua", teclas, 4, 40);
    check(err.empty(), (std::string("tres en raya: 40 vueltas") + (err.empty() ? "" : ": " + err)).c_str());
  }
  {
    // El reloj no tiene partida, pero sí el caso que importa: muchos ticks
    // seguidos sin que el minuto cambie no tienen que hacer nada raro.
    const char* teclas[] = {"ok"};
    const std::string err = playApp("examples/Apps/reloj.lua", teclas, 1, 50);
    check(err.empty(), (std::string("reloj: 50 vueltas") + (err.empty() ? "" : ": " + err)).c_str());
  }

  printf("\n%s (%d fallas)\n", failures == 0 ? "TODO BIEN" : "HAY FALLAS", failures);
  return failures == 0 ? 0 : 1;
}
