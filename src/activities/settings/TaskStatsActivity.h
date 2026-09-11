#pragma once

#include "activities/Activity.h"

// Ajustes → Sistema → Memoria. La contracara de `src/TaskConfig.h`: ahí se
// DECLARA cuánto stack le toca a cada tarea, y acá se ve cuánto usó de verdad.
//
// Un presupuesto que nadie mide no es un presupuesto, es un comentario. Esta
// pantalla es la que distingue "va sobrado" de "va justo" sin cable ni
// depurador: por cada tarea, el stack declarado, el máximo que llegó a usar y
// cuánto le sobró en el peor momento; y arriba, el heap interno (el que se
// acaba primero) con su mínimo histórico y el bloque contiguo más grande, que
// es el número que decide si una asignación grande va a entrar o no.
//
// También muestra el reposo (`IdleSleep`): cuántos ciclos lleva y cuánto tiempo
// estuvo en light sleep, que es lo único que dice si la etapa del medio está
// funcionando o si algo la bloquea siempre.
//
// Se refresca sola cada REFRESH_MS mientras esté abierta, pero sólo repinta si
// algún número cambió lo suficiente: en tinta, repintar por un byte es un
// fantasma cada dos segundos.
class TaskStatsActivity final : public Activity {
 public:
  explicit TaskStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TaskStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr unsigned long REFRESH_MS = 2000;
  // Debajo de esto el cambio es ruido de asignaciones normales: repintar por
  // eso gasta tinta y no dice nada nuevo.
  static constexpr uint32_t REPAINT_BYTES = 2048;

  struct Snapshot {
    uint32_t internalFree = 0;
    uint32_t internalMin = 0;
    uint32_t internalBlock = 0;
    uint32_t psramFree = 0;
    uint32_t psramBlock = 0;
    uint32_t stackFree[8] = {};
    uint32_t restCycles = 0;
  };
  static Snapshot take();
  static bool worthRepainting(const Snapshot& a, const Snapshot& b);

  Snapshot painted;
  unsigned long lastPaint = 0;

  // --- El medidor de OK mantenido ---------------------------------------
  //
  // Hasta 1.5.48 se daba por sentado que "OK largo NUNCA llega" y por eso
  // ninguna función colgaba de ahí. La razón era real hasta 1.5.46 (OK era
  // confirm y power compartidos y el SDK no levantaba el bit mientras se
  // mantenía), pero dejó de serlo en 1.5.47 y nadie lo volvió a probar.
  // Esto lo contesta sin cable: mantené OK acá y la pantalla dice cuánto lo
  // tuviste apretado y si el evento de pulsación larga llegó de verdad.
  static constexpr unsigned long OK_HOLD_TEST_MS = 1000;
  bool okDown = false;         // estaba apretado en la pasada anterior
  bool okFired = false;        // el evento llegó durante ESTA pulsación
  unsigned long okHoldMs = 0;  // cuánto lleva apretado ahora
  unsigned long lastOkHoldMs = 0;  // lo que duró la última pulsación
  bool lastOkFired = false;        // si en esa llegó el evento
  bool sawOkHold = false;          // ya hubo al menos una, hay algo que mostrar
};
