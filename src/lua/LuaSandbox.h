#pragma once

#include <cstddef>

struct lua_State;

// El cajón donde corren las apps de la tarjeta, sin nada del aparato adentro.
//
// Está separado de `LuaApp` a propósito: acá no se incluye Arduino, ni el
// renderer, ni la tarjeta. Así la política del cajón (qué bibliotecas se abren,
// qué se saca, cuánta memoria y cuántas instrucciones se permiten) se puede
// PROBAR de escritorio con g++, que es la única forma de estar seguro de que
// `io`, `os` y `load` no están cuando uno cree que no están. La prueba vive en
// test/lua_sandbox/.
namespace luasandbox {

// Crea el intérprete con el tope de memoria puesto. En el aparato la memoria
// sale de PSRAM; de escritorio, del malloc de siempre. Devuelve nullptr si no
// hay memoria.
lua_State* create(size_t memCap);
void destroy(lua_State* L);

// Bytes que el intérprete tiene pedidos ahora mismo.
size_t memUsed();

// Corta la llamada en curso con un error de Lua después de `steps`
// instrucciones. Es lo que convierte un `while true do end` de una app en un
// error de la app en vez de un aparato colgado.
void armStepLimit(lua_State* L, int steps);
void clearStepLimit(lua_State* L);

}  // namespace luasandbox
