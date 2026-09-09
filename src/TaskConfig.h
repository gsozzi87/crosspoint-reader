#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

// Un solo lugar para las tareas FreeRTOS propias: en qué núcleo corren, con qué
// prioridad y con cuánto stack. Antes cada archivo tenía sus números sueltos y
// nadie podía ver el mapa completo (idea tomada del `task_config` de
// folloup-sticky, que hace exactamente esto para la misma placa).
//
// Reparto: el loop de Arduino (la UI) vive en el núcleo 1; todo lo que pueda
// frenar la UI va al 0. El SDK crea las suyas aparte y NO se tocan desde acá:
//   audio_play  (AudioManager::play)  prio 10, core 0, 8 KB — una por reproducción
//   fi_input    (InputManager::beginAsync) no se usa en esta placa
//   ble-conn    sólo con BLE
namespace tasks {

// Núcleos.
constexpr BaseType_t CORE_UI = 1;      // loop de Arduino y render
constexpr BaseType_t CORE_AUDIO = 0;   // reproducción, clics y red del IDF

// Render de las Activities (ActivityManager). Misma prioridad que el loop de
// Arduino: comparten el núcleo por time-slicing y un render largo (HALF ≈ 1,7 s)
// no puede matar de hambre a la lectura de botones.
constexpr const char* RENDER_NAME = "ActivityManagerRender";
constexpr uint32_t RENDER_STACK = 8192;
constexpr UBaseType_t RENDER_PRIO = 1;

// Clics de la interfaz (UiSound): por debajo del audio del SDK (10) para que
// nunca le corte una frase a la voz ni a la música.
constexpr const char* UI_SOUND_NAME = "ui_sound";
constexpr uint32_t UI_SOUND_STACK = 4096;
constexpr UBaseType_t UI_SOUND_PRIO = 4;

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
