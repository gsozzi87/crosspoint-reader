#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "voice/AlertBeep.h"

// Simón (repetir la secuencia). Cuatro casilleros grandes en 2x2, cada uno con
// una figura distinta (círculo, cuadrado, triángulo y rombo: la pantalla es
// blanco y negro, así que no hay colores que valgan). El aparato muestra la
// secuencia encendiendo un casillero por vez —relleno en negro con la figura en
// blanco— y el jugador la repite. Cada ronda agrega un paso y acorta un poco los
// tiempos, con el piso que pide la tinta electrónica (500 ms encendido / 250 ms
// apagado). El récord de la sesión vive en un miembro estático, así sobrevive a
// salir y volver a entrar al juego sin tocar la SD.
//
// Control (cuatro botones, sin teclado ni pantalla táctil):
//   - Pantalla de inicio y fin de partida: OK empieza una partida nueva.
//   - Mientras la máquina muestra la secuencia no se acepta nada: hay que mirar.
//   - Tu turno: la palanca (ARRIBA/ABAJO) mueve el cursor entre los cuatro
//     casilleros en círculo y OK confirma el elegido. Al confirmar, el casillero
//     se enciende un momento como devolución.
//   - Atrás sale del juego; Atrás mantenido 1 s abandona la partida y vuelve a la
//     pantalla de inicio. OK no tiene pulsación larga en esta placa (apaga).
//
// Sonido: solo un pitido corto al equivocarse (`AlertBeep`, arrancado y frenado
// desde loop() con millis(), nunca bloqueando). Los casilleros son mudos: cada
// arranque del pitido arma un patrón de 1,6 s en PSRAM y no sirve para tonos
// cortos por casillero.
class SimonActivity final : public Activity {
 public:
  explicit SimonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Simon", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t {
    READY,  // pantalla de inicio: OK arranca
    SHOW,   // la máquina muestra la secuencia
    PLAYING,  // turno del jugador (INPUT es un macro de esp32-hal-gpio.h, no se puede usar)
    FLASH,  // el casillero recién elegido queda encendido un momento
    ROUND,  // acertó la ronda entera: cartel y a la siguiente
    OVER    // se equivocó (o llegó al final): resumen, OK juega de nuevo
  };

  static constexpr int PAD_COUNT = 4;
  static constexpr int MAX_LEN = 60;  // llegar hasta acá es ganar

  static constexpr int PARTIALS_BEFORE_CLEAN = 10;  // regla del panel: limpio cada 10-15 parciales

  // Tiempos (ms). Los pisos son los que pide la tinta electrónica.
  static constexpr unsigned long ON_MIN_MS = 500;
  static constexpr unsigned long OFF_MIN_MS = 250;
  static constexpr unsigned long ON_START_MS = 700;
  static constexpr unsigned long OFF_START_MS = 380;
  static constexpr unsigned long LEAD_IN_MS = 600;   // pausa antes del primer casillero
  static constexpr unsigned long FLASH_MS = 320;     // devolución al elegir
  static constexpr unsigned long ROUND_MS = 800;     // cartel de ronda acertada
  static constexpr unsigned long WRONG_BEEP_MS = 1200;
  static constexpr unsigned long RESTART_HOLD_MS = 1000;

  // Partida
  State state = State::READY;
  std::array<uint8_t, MAX_LEN> sequence{};
  int length = 0;      // pasos de la secuencia actual (= nivel)
  int showIndex = 0;   // paso que se está mostrando
  bool padLit = false; // en SHOW: casillero encendido o hueco entre pasos
  int inputIndex = 0;  // paso que le toca repetir al jugador
  int cursor = 0;      // casillero bajo el cursor
  int pressed = -1;    // casillero encendido por la devolución de FLASH
  bool pressedOk = false;
  int score = 0;       // aciertos de toda la partida
  bool won = false;    // llegó a MAX_LEN
  unsigned long stepAt = 0;

  // Pitido del error
  AlertBeep beep;
  bool beeping = false;
  unsigned long beepSince = 0;

  int partialCount = 0;
  bool forceClean = false;
  ButtonNavigator buttonNavigator;

  static uint16_t bestLevel;  // mejor nivel de la sesión

  // Partida
  void startGame();
  void addStep();
  void beginShow();
  void advanceShow();
  void pressPad();
  void resolveFlash();
  void endGame(bool victory);
  unsigned long onMs() const;
  unsigned long offMs() const;

  // Dibujo
  void drawInfoBar(int y) const;
  void drawBoard(int top, int bottom) const;
  void drawPad(int idx, int x, int y, int size, bool lit, bool showCursor) const;
  void drawSummary(int top, int bottom) const;

  // Primitivas propias (el renderer no trae círculos ni rombos)
  void fillCircle(int cx, int cy, int r, bool ink) const;
  void fillDiamond(int cx, int cy, int r, bool ink) const;
  void drawFigure(int pad, int cx, int cy, int r, bool ink) const;
};
