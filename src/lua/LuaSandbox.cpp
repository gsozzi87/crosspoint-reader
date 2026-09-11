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
  const size_t after = g_used - (ptr ? osize : 0) + nsize;
  if (after > g_cap) return nullptr;
  void* out = rawRealloc(ptr, nsize);
  if (!out) return nullptr;
  g_used = after;
  return out;
}

void stepHook(lua_State* L, lua_Debug*) { luaL_error(L, "la app tardó demasiado en una sola llamada"); }
}  // namespace

size_t memUsed() { return g_used; }

lua_State* create(const size_t memCap) {
  g_used = 0;
  g_cap = memCap;
  lua_State* L = lua_newstate(alloc, nullptr);
  if (!L) return nullptr;

  // SÓLO estas. Nada de io, os, package ni debug: una app no puede abrir un
  // archivo, salir a la red, cargar código nativo ni mirar el estado del
  // intérprete. Los .c de esas bibliotecas ni siquiera están en lib/Lua.
  static const luaL_Reg SAFE_LIBS[] = {
      {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
      {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine},
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
  lua_close(L);
  g_used = 0;
}

void armStepLimit(lua_State* L, const int steps) { lua_sethook(L, stepHook, LUA_MASKCOUNT, steps); }
void clearStepLimit(lua_State* L) { lua_sethook(L, nullptr, 0, 0); }

}  // namespace luasandbox
