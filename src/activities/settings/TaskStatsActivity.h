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
};
