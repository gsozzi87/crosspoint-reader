#pragma once

#include "activities/Activity.h"
#include "input/MotionInput.h"

// Ajustes → Movimiento. Tres cosas en una pantalla:
//
//  * qué está leyendo el sensor AHORA (los tres ejes en g, ya en coordenadas de
//    pantalla, y la magnitud), que es la única forma de saber si el chip está
//    vivo sin un depurador;
//  * qué gesto reconoció último, para probarlos sin tener que ir a buscar el
//    recordatorio que suena;
//  * calibrar los ejes, porque cómo está montado el sensor en la placa no está
//    documentado y sin eso "inclinar a la derecha" puede ser cualquier cosa.
//
// El aparato no tiene teclado: la calibración es guiada, con tres posiciones y
// OK en cada una.
class MotionActivity final : public Activity {
 public:
  explicit MotionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Motion", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Calibrando hay que poder dejar el aparato apoyado sin que se duerma.
  bool preventAutoSleep() override { return state != LIVE; }

 private:
  enum State {
    LIVE,          // valores y último gesto
    CAL_FLAT,      // apoyado boca arriba
    CAL_RIGHT,     // inclinado a la derecha
    CAL_TOWARD,    // inclinado hacia el usuario
    CAL_DONE,      // listo
  };
  State state = LIVE;
  bool calFailed = false;
  MotionInput::Event shown = MotionInput::Event::None;
  unsigned long shownAt = 0;
  unsigned long lastPaint = 0;
  int partialCount = 0;
};
