#include "LuaSandbox.h"

#include <initializer_list>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#else
#include <cstdlib>
#endif

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace luasandbox {

// Definidas más abajo; `destroy` las necesita.
void armStepLimit(lua_State* L, int steps);

namespace {
// Una app por vez, así que el contador es uno solo. Si algún día hay dos, esto
// pasa a ser el `ud` del allocator.
size_t g_used = 0;
size_t g_cap = 0;

void* rawRealloc(void* ptr, const size_t size) {
#if defined(ESP_PLATFORM)
  // El intérprete vive en PSRAM: sobra, y así una app no le come la memoria
  // interna al TLS ni a los buffers de I2S. Si no hay PSRAM se cae a la interna.
  void* out = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
  if (!out) out = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL);
  return out;
#else
  return realloc(ptr, size);
#endif
}

void rawFree(void* ptr) {
#if defined(ESP_PLATFORM)
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

// Una vez que la app se pasó del tope, no hay vuelta atrás: el asignador deja
// de dar memoria. Ver `stepHook` para por qué hace falta esto además del error.
bool g_hardStop = false;
// Cuántas veces saltó la guardia dentro de la MISMA llamada. Se pone en cero en
// cada `armStepLimit`, o sea al empezar cada callback de la app.
int g_trips = 0;

// Con tope: una tabla que crece sin parar tiene que morirse ella (Lua convierte
// el nullptr en "not enough memory"), no llevarse puesto el resto del aparato.
void* alloc(void*, void* ptr, const size_t osize, const size_t nsize) {
  if (nsize == 0) {
    if (ptr) {
      g_used -= osize;
      rawFree(ptr);
    }
    return nullptr;
  }
  // Con la app ya condenada no se le da un byte más. Liberar sí, pedir no.
  if (g_hardStop) return nullptr;
  const size_t after = g_used - (ptr ? osize : 0) + nsize;
  if (after > g_cap) return nullptr;
  void* out = rawRealloc(ptr, nsize);
  if (!out) return nullptr;
  g_used = after;
  return out;
}

// POR QUÉ NO ALCANZA CON `luaL_error`: lo que tira es un error de Lua común y
// corriente, así que `pcall` —que está en la base y tiene que estar— lo ATRAPA.
// Una app con
//
//     while true do pcall(function() while true do end end) end
//
// se comía el error en cada vuelta y seguía para siempre. Y como
// `tasks::runBounded` espera al worker sin tope (`portMAX_DELAY`), el que se
// colgaba no era la app: era el loop de Arduino. Sin recordatorios, sin reposo,
// sin salida salvo cortar la corriente. La promesa escrita en `LuaApp.h` —"un
// `while true do end` termina en error de la app, no en un aparato colgado"—
// valía sólo para el caso ingenuo.
//
// El arreglo es cerrar la puerta por donde Lua NO puede seguir: la memoria. Al
// pasarse del tope se marca `g_hardStop` y el asignador empieza a devolver
// nullptr. A partir de ahí Lua no puede ni armar el objeto de error ni crecer
// la pila para entrar a un `pcall`, así que la llamada termina sí o sí (con
// LUA_ERRMEM) por más `pcall` anidados que tenga la app. El error de acá abajo
// sigue estando para el caso normal, que da un mensaje entendible.
//
// Va en DOS tiempos para no perder el mensaje entendible: el primer golpe tira
// el error de siempre (ahí todavía hay memoria para armar el texto) y re-arma la
// guardia corta. Si el error llega al host, listo — es el caso normal y el
// usuario lee "la app tardó demasiado". Si en cambio la guardia vuelve a saltar
// enseguida, es que alguien se lo comió: ahí sí se condena al intérprete.
constexpr int SWALLOWED_STEPS = 10000;

void stepHook(lua_State* L, lua_Debug*) {
  ++g_trips;
  if (g_trips >= 2) {
    g_hardStop = true;
  } else {
    lua_sethook(L, stepHook, LUA_MASKCOUNT, SWALLOWED_STEPS);
  }
  luaL_error(L, "la app tardó demasiado en una sola llamada");
}
}  // namespace

size_t memUsed() { return g_used; }

lua_State* create(const size_t memCap) {
  g_used = 0;
  g_cap = memCap;
  g_hardStop = false;
  lua_State* L = lua_newstate(alloc, nullptr);
  if (!L) return nullptr;

  // SÓLO estas. Nada de io, os, package ni debug: una app no puede abrir un
  // archivo, salir a la red, cargar código nativo ni mirar el estado del
  // intérprete. Los .c de esas bibliotecas ni siquiera están en lib/Lua.
  static const luaL_Reg SAFE_LIBS[] = {
      {LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
      {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
  };
  for (const luaL_Reg& lib : SAFE_LIBS) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }

  // De la base quedan afuera las que leen del disco o compilan texto: con
  // `load` una app podría armar código en tiempo de ejecución y saltearse
  // cualquier revisión de lo que dice el archivo.
  for (const char* gone : {"dofile", "loadfile", "load", "loadstring", "require", "collectgarbage"}) {
    lua_pushnil(L);
    lua_setglobal(L, gone);
  }
  // string.dump serializa funciones a bytecode. Sin `load` no sirve para nada,
  // pero tampoco tiene por qué estar.
  lua_getglobal(L, LUA_STRLIBNAME);
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);
  return L;
}

void destroy(lua_State* L) {
  if (!L) return;
  // `lua_close` no es sólo liberar memoria: corre los finalizadores `__gc` de
  // la app, o sea CÓDIGO DE LA APP. Y se lo llama desde el loop de Arduino, no
  // desde el worker de `runBounded`, así que un `__gc` con un bucle infinito
  // colgaba el aparato justo en la salida — la puerta de emergencia. Se arma la
  // guardia también acá: cada finalizador queda acotado igual que una llamada
  // normal.
  armStepLimit(L, 400000);
  lua_close(L);
  g_used = 0;
  g_hardStop = false;
}

bool hardStopped() { return g_hardStop; }

void armStepLimit(lua_State* L, const int steps) {
  g_trips = 0;
  lua_sethook(L, stepHook, LUA_MASKCOUNT, steps);
}
void clearStepLimit(lua_State* L) { lua_sethook(L, nullptr, 0, 0); }

}  // namespace luasandbox
