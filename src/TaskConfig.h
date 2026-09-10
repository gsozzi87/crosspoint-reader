#pragma once

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>

// Un solo lugar para las tareas FreeRTOS: en qué núcleo corren, con qué
// prioridad, con cuánto stack y para qué. Antes cada archivo tenía sus números
// sueltos y nadie podía ver el mapa completo (la idea es del `task_config` de
// folloup y del reparto de workers acotados de rustmix-wave, los dos firmwares
// que corren en esta misma placa).
//
// La regla es que un número declarado acá se pueda comparar con lo que de
// verdad se usó: cada tarea se anota al arrancar (`attach`) y `usage()` devuelve
// el stack libre que le quedó en el peor momento. Eso es lo que muestra
// Ajustes → Sistema → Memoria, y es lo único que distingue "va justo" de "va
// sobrado" sin adivinar.
//
// Reparto: el loop de Arduino (la UI) vive en el núcleo 1; todo lo que pueda
// frenar la UI va al 0.
namespace tasks {

// Núcleos.
constexpr BaseType_t CORE_UI = 1;     // loop de Arduino y render
constexpr BaseType_t CORE_AUDIO = 0;  // reproducción, clics y red del IDF

enum class Id : uint8_t {
  Loop,       // el loop de Arduino: la UI entera
  Render,     // ActivityManager
  UiSound,    // clics de la interfaz
  AudioPlay,  // del SDK (AudioManager), no la creamos nosotros
  Worker,     // el worker acotado de turno (runBounded)
  COUNT,
};

struct Budget {
  const char* name;
  uint32_t stack;   // bytes declarados
  UBaseType_t prio;
  BaseType_t core;  // -1 = sin fijar
  const char* what;
};

// Presupuestos declarados. El orden es el de `Id`.
inline const Budget& budget(const Id id) {
  static const Budget table[] = {
      // El loop de Arduino no lo creamos nosotros: el stack sale del sdkconfig
      // del framework (CONFIG_ARDUINO_LOOP_STACK_SIZE). Se declara igual porque
      // es el que más cerca está del límite: por ahí pasan el TLS, el parseo de
      // EPUB y todo lo que no tiene tarea propia.
      {"loopTask", 8192, 1, CORE_UI, "UI, entrada, red sincrónica"},
      // Misma prioridad que el loop: comparten núcleo por time-slicing y un
      // render largo (HALF ~ 1,7 s) no puede matar de hambre a los botones.
      {"ActivityManagerRender", 8192, 1, CORE_UI, "pintado de las pantallas"},
      // Por debajo del audio del SDK (10) para que nunca le corte una frase a
      // la voz ni a la música.
      {"ui_sound", 4096, 4, CORE_AUDIO, "clics de la interfaz"},
      {"audio_play", 8192, 10, CORE_AUDIO, "reproducción (del SDK)"},
      {"worker", 0, 3, CORE_AUDIO, "trabajo pesado de vida corta"},
  };
  return table[static_cast<uint8_t>(id)];
}

// Nombres y números que usan los creadores de tareas (se mantienen para que el
// sitio de creación siga leyéndose solo).
constexpr const char* RENDER_NAME = "ActivityManagerRender";
constexpr uint32_t RENDER_STACK = 8192;
constexpr UBaseType_t RENDER_PRIO = 1;
constexpr const char* UI_SOUND_NAME = "ui_sound";
constexpr uint32_t UI_SOUND_STACK = 4096;
constexpr UBaseType_t UI_SOUND_PRIO = 4;

// --- Registro vivo -------------------------------------------------------

inline TaskHandle_t* handleSlot(const Id id) {
  static TaskHandle_t slots[static_cast<uint8_t>(Id::COUNT)] = {};
  return &slots[static_cast<uint8_t>(id)];
}

// La tarea se anota a sí misma al arrancar. Sin esto no hay con qué medir: el
// handle es lo único que da la marca de agua del stack.
inline void attach(const Id id, const TaskHandle_t handle = nullptr) {
  *handleSlot(id) = handle ? handle : xTaskGetCurrentTaskHandle();
}
inline void detach(const Id id) { *handleSlot(id) = nullptr; }

struct Usage {
  const Budget* budget = nullptr;
  bool alive = false;
  uint32_t freeBytes = 0;  // lo que le sobró en el peor momento
  uint32_t usedBytes = 0;  // declarado - libre (0 si no se conoce el declarado)
};

inline Usage usage(const Id id) {
  Usage u;
  u.budget = &budget(id);
  TaskHandle_t h = *handleSlot(id);
#if INCLUDE_xTaskGetHandle
  // Las del SDK no se anotan solas (audio_play nace y muere con cada
  // reproducción): se busca por nombre, que es la única forma de verlas vivas.
  if (!h && u.budget->name) h = xTaskGetHandle(u.budget->name);
#endif
  if (!h) return u;
  u.alive = true;
  u.freeBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(h)) * sizeof(StackType_t);
  if (u.budget->stack > u.freeBytes) u.usedBytes = u.budget->stack - u.freeBytes;
  return u;
}

// --- Worker acotado ------------------------------------------------------

// Corre `fn` en una tarea propia con el stack que se le declare y espera a que
// termine. NO es para no frenar la UI (el llamador se queda esperando, igual
// que antes): es para que un stack grande exista SOLO durante ese trabajo en
// vez de estar reservado para siempre en el loop, que es el que se queda corto.
//
// Devuelve false si no se pudo crear la tarea (sin heap interno suficiente): el
// llamador tiene que poder seguir sin el worker. `usedOut`, si se pasa, recibe
// cuántos bytes de stack se usaron de verdad, que es el número con el que se
// ajusta el presupuesto la próxima vez.
bool runBounded(const char* name, uint32_t stackBytes, void (*fn)(void*), void* arg, uint32_t* usedOut = nullptr,
                UBaseType_t prio = 3, BaseType_t core = CORE_AUDIO);

// Instantánea de memoria para el log: se llama cada 10 s desde el loop y en los
// puntos donde la presión importa (antes de WiFi, antes de una grabación
// larga). La marca de agua mínima (`minFree`) es lo que dice si en algún
// momento estuvimos cerca de quedarnos sin heap interno.
inline void logMemory(const char* where) {
  const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t internalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const size_t internalMax = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t psramMax = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  LOG_INF("MEM", "%s: interna libre %u (min %u, bloque %u) · psram libre %u (bloque %u) · stack libre %u", where,
          (unsigned)internalFree, (unsigned)internalMin, (unsigned)internalMax, (unsigned)psramFree, (unsigned)psramMax,
          (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
}

}  // namespace tasks
