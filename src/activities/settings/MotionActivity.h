#pragma once

#include "activities/Activity.h"
#include "input/MotionInput.h"

// Ajustes → Movimiento. Es la pantalla de INSTRUMENTO del aparato: la única
// forma de saber, sin cable ni depurador, si el IMU está vivo, cómo está
// montado y qué gesto acaba de reconocer.
//
// Cinco bloques, de arriba a abajo:
//
//  * el visor: la magnitud del vector aceleración en dígitos de segmentos (se
//    lee de lejos, apoyando el aparato en la mesa) y el estado del motor de
//    gestos y del de golpes;
//  * seis medidores bipolares (ax/ay/an y gx/gy/gn) de 18 px: a 8 px, en tinta,
//    un medidor es un hilo que no se ve;
//  * el último gesto reconocido y cuántos van de cada tipo desde que se abrió
//    la pantalla, que es lo que permite probar un gesto sin adivinar;
//  * cómo está montado el sensor (eje normal a la pantalla) y cómo calibrarlo;
//  * los umbrales con los que trabaja `MotionInput`, como referencia de lectura
//    de los medidores de arriba.
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
    LIVE,          // instrumento
    CAL_FLAT,      // apoyado boca arriba
    CAL_RIGHT,     // inclinado a la derecha
    CAL_TOWARD,    // inclinado hacia el usuario
    CAL_DONE,      // listo
  };
  // Los cinco contadores del panel EVENTO. Las cuatro inclinaciones cuentan
  // como una sola: lo que se está probando es el gesto, no la dirección.
  enum Counter { CNT_TILT, CNT_SHAKE, CNT_ROTATE, CNT_LEVEL, CNT_TAP, CNT_COUNT };

  void renderLive(int width, int bottom) const;
  void renderCalibration(int width, int height, int bottom) const;
  // Encabezado de sección: etiqueta en UI_10 negrita, referencia a la derecha
  // en SMALL y una regla de 1 px. Devuelve el y donde empieza el contenido.
  int drawSection(int y, int width, const char* label, const char* right) const;
  // Una fila de medidor: etiqueta, valor y la barra bipolar. Devuelve el
  // siguiente y.
  int drawMeterRow(int y, int width, const char* label, float value, float fullScale, bool valid) const;
  void countEvent(MotionInput::Event e);
  // Regla del panel: sólo se repinta si algo se movió de verdad.
  bool worthRepainting() const;

  State state = LIVE;
  bool calFailed = false;
  MotionInput::Event shown = MotionInput::Event::None;
  unsigned long lastPaint = 0;
  int counters[CNT_COUNT] = {0, 0, 0, 0, 0};
  // Lo que se dibujó la última vez. Un número grande que cambia dos veces por
  // segundo es fantasma seguro, así que el repintado se compara contra esto y
  // el aparato quieto no gasta un solo refresco.
  MotionInput::Reading painted;
};
