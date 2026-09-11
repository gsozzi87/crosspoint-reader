#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

#include "activities/Activity.h"

// 2048: juntar dos números iguales para hacer el siguiente. Es por turnos de
// nacimiento, así que es de los pocos juegos "modernos" que le caen bien a un
// panel de tinta electrónica: una jugada, una repintada.
//
// Control, sin teclado:
//   - Con el sensor: se inclina el aparato hacia donde se quiere empujar.
//   - Sin sensor: la palanca gira la flecha y OK empuja.
//   - Atrás sale. Atrás mantenido 1 s empieza otra partida.
//
// El tablero es de 4x4 y los números van en cajas: el valor se dibuja con el
// tamaño de letra que entre, y el fondo se trama más oscuro cuanto más alto el
// número, que en blanco y negro es la única forma de que se vea el progreso de
// un vistazo. El número NUNCA cae sobre la trama: lleva un plato blanco debajo
// (`drawTextPlate`), que es la regla del rediseño para todo texto sobre algo
// tramado.
class Game2048Activity final : public Activity {
 public:
  explicit Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Game2048", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Dir : uint8_t { Up, Right, Down, Left };
  static constexpr int N = 4;

  void reset();
  void spawn();
  bool slide(Dir d);          // true si algo se movió
  bool movesLeft() const;
  void layout();
  void drawBoard();
  void drawAim(int cx, int cy) const;
  void drawInfo();

  std::array<uint16_t, N * N> grid{};
  int score = 0;
  int best = 0;
  bool over = false;
  Dir aim = Dir::Left;
  bool useMotion = false;

  // El bloque de abajo se arma de abajo hacia arriba y con alto fijo: el
  // tablero no cambia de tamaño cuando cambia el texto de la ayuda.
  int originX = 0, originY = 0, cell = 0;
  int statusTop = 0, statsTop = 0, helpTop = 0;
  int partialCount = 0;
  bool forceClean = true;
  unsigned long backHeldSince = 0;
};
