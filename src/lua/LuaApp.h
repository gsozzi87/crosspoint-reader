#pragma once

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
class LuaApp {
 public:
  // Cuánto stack se le da al worker donde corre el script. 32 KB es lo mismo
  // que le da rustmix a su cargador de Lua.
  static constexpr uint32_t STACK = 32768;
  // Techo de memoria del intérprete. Sale de PSRAM (sobra) pero con tope, o una
  // tabla que crece sin parar se lleva puesto todo lo demás.
  static constexpr size_t MEM_CAP = 192 * 1024;
  // Instrucciones antes de cortar una llamada. Un `while true do end` tiene que
  // terminar en un error de la app, no en un aparato colgado.
  static constexpr int STEP_LIMIT = 400000;
  static constexpr unsigned long TICK_MS = 120;
  // Tope del texto que una app puede guardar (`cp.save`).
  static constexpr size_t SAVE_CAP = 4096;

  // Una app instalada: el archivo y el nombre que se muestra.
  struct Entry {
    std::string path;
    std::string name;
  };
  static std::vector<Entry> installed();
  static const char* dir();

  ~LuaApp();

  bool open(GfxRenderer& renderer, const std::string& path);
  void close();

  bool ok() const { return state_ != nullptr && error_.empty(); }
  const std::string& error() const { return error_; }
  const std::string& name() const { return name_; }
  bool quitRequested() const { return quit_; }

  // Las tres llamadas al script. Devuelven true si hay que repintar. Un error
  // adentro deja `error()` cargado y la app se da por terminada.
  bool onKey(const char* key);
  bool onTick();
  void onDraw();

 private:
  bool callback(const char* fn, const char* arg);

  lua_State* state_ = nullptr;
  std::string error_;
  std::string name_;
  std::string path_;
  bool quit_ = false;
  bool hasTick_ = false;
};
