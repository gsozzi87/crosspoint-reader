#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// La viborita, sobre una grilla de 14x20 casilleros grandes (32 px): come,
// crece, acelera y se muere contra la pared o contra su propia cola.
//
// Control (cuatro botones, sin teclado ni pantalla táctil; ARRIBA y ABAJO son
// una palanca física: arriba XOR abajo, nunca las dos). Con dos direcciones de
// entrada el control clásico es GIRAR, no apuntar:
//   - Palanca ARRIBA: gira a la IZQUIERDA. Palanca ABAJO: gira a la DERECHA.
//     La viborita avanza sola; la cabeza lleva una punta blanca que dice para
//     dónde va, y el giro se ve en el acto aunque el paso todavía no llegue.
//   - OK: pausa (y desde la pausa, sigue). OK no tiene pulsación larga en esta
//     placa: mantenerlo apaga el aparato.
//   - Atrás: desde el juego pausa (nunca se pierde la partida por un toque);
//     desde la pausa, el inicio o el final, sale.
//
// Tinta electrónica: cada paso es un refresco, así que el paso más rápido es de
// 250 ms y el más lento de 340 ms, y sobre todo se repintan SOLO los casilleros
// que cambian (la cabeza nueva, la vieja que pasa a ser cuerpo, la cola que se
// va y la comida). La pantalla entera se rehace al entrar, al cambiar de estado
// y cada 12 parciales (regla del panel), que es cuando además se manda un
// HALF_REFRESH para que no fantasmee.
class SnakeActivity final : public Activity {
 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Snake", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Mientras se juega la viborita avanza sola: sin esto el aparato baja el reloj
  // por inactividad y se duerme en medio de la partida. Al morir vuelve a dormir
  // normalmente, así que no queda despierto para siempre.
  bool preventAutoSleep() override { return state == State::PLAYING; }

 private:
  enum class State : uint8_t {
    START,    // pantalla de inicio: qué hace cada botón
    PLAYING,  // la viborita avanza
    PAUSED,   // pausa, con la opción de salir
    OVER      // resumen final: OK juega de nuevo
  };

  static constexpr int COLS = 14;
  static constexpr int ROWS = 20;
  static constexpr int CAP = COLS * ROWS;  // largo máximo posible
  static constexpr int MAX_CELL = 34;
  static constexpr int MAX_DIRTY = 16;  // más que esto y conviene repintar todo
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel
  static constexpr unsigned long STEP_START_MS = 340;
  static constexpr unsigned long STEP_MIN_MS = 250;
  static constexpr unsigned long STEP_LEVEL_MS = 15;
  static constexpr int FOOD_PER_LEVEL = 5;
  static constexpr int START_LENGTH = 4;

  // --- partida ---
  State state = State::START;
  std::array<uint16_t, CAP> body{};    // buffer circular: de la cola a la cabeza
  std::array<uint8_t, CAP> occupied{};  // 1 = hay cuerpo en ese casillero
  int headPos = 0;  // índice de la cabeza dentro de `body`
  int tailPos = 0;  // índice de la cola dentro de `body`
  int length = 0;
  int dir = 0;  // 0 arriba, 1 derecha, 2 abajo, 3 izquierda
  int headCol = 0;
  int headRow = 0;
  int foodIdx = -1;
  long score = 0;
  int eaten = 0;
  int level = 1;
  bool won = false;  // llenó la pantalla entera
  static long bestScore;  // mejor de la sesión (no toca la SD)
  unsigned long lastStep = 0;

  // --- pintura ---
  std::array<uint16_t, MAX_DIRTY> dirty{};
  int dirtyCount = 0;
  bool infoDirty = false;
  bool fullRepaint = true;
  int partialCount = 0;
  ButtonNavigator buttonNavigator;
  // Geometría, recalculada igual en cada render para que el repintado parcial
  // caiga exactamente donde cayó el completo.
  int cellPx = 0;
  int boardX = 0;
  int boardY = 0;
  int infoY = 0;

  // Partida
  void startGame();
  void step();
  void turn(int delta);
  void placeFood();
  void gameOver(bool win);
  unsigned long stepIntervalMs() const;
  void markDirty(int idx);

  // Dibujo
  void layout();
  void fillCircle(int cx, int cy, int r, bool on) const;
  void drawCell(int idx) const;
  void drawSegment(int x, int y, bool head) const;
  void drawFood(int x, int y) const;
  void drawBoardFrame() const;
  void drawInfo() const;
  void drawOverlay() const;
  void drawHints() const;
};
