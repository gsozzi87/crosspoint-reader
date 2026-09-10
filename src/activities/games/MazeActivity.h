#pragma once

#include <Arduino.h>

#include <cstdint>
#include <vector>

#include "activities/Activity.h"

// Laberinto: sacar la bolita desde arriba a la izquierda hasta la salida de
// abajo a la derecha.
//
// POR TURNOS, como todos los juegos de esta placa: un refresco parcial del panel
// tarda ~30 ms y cada doce hay que hacer uno completo, así que nada se mueve de
// corrido. Una tirada = la bolita rueda en esa dirección hasta que choca, igual
// que una bola en una bandeja inclinada, y ahí se repinta una sola vez. Con eso
// una partida entera son treinta o cuarenta refrescos, no mil.
//
// Control, sin teclado:
//   - Con el sensor de movimiento: se inclina el aparato hacia donde se quiere
//     tirar. Un gesto, una tirada.
//   - Sin sensor (o con los gestos apagados): la palanca gira la flecha de
//     dirección y OK tira.
//   - Atrás sale. Atrás mantenido 1 s empieza otro laberinto.
class MazeActivity final : public Activity {
 public:
  explicit MazeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Maze", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Paredes de una celda, como bits: el laberinto se guarda por celda y no por
  // línea, así una pared es siempre la misma de los dos lados.
  enum Wall : uint8_t { UP_W = 1, RIGHT_W = 2, DOWN_W = 4, LEFT_W = 8 };
  enum class Dir : uint8_t { Up, Right, Down, Left };

  void generate();
  bool canMove(int cx, int cy, Dir d) const;
  void roll(Dir d);
  void layout();
  void drawMaze();
  void drawInfo();

  int cols = 9;
  int rows = 15;
  std::vector<uint8_t> cells;  // cols*rows máscaras de Wall
  int ballX = 0, ballY = 0;
  int goalX = 0, goalY = 0;
  int level = 1;
  int moves = 0;
  bool won = false;
  Dir aim = Dir::Right;  // la flecha, para el modo sin sensor
  bool useMotion = false;

  // Geometría calculada en cada pintada (depende del tema).
  int originX = 0, originY = 0, cell = 0;
  int partialCount = 0;
  bool forceClean = true;
  unsigned long backHeldSince = 0;
};
