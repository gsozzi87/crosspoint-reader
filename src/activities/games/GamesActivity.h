#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Menú de juegos del hub. Cada juego es una Activity suya en src/activities/games/
// y se abre desde acá; todos usan los mismos cuatro botones: ARRIBA/ABAJO
// (palanca), OK (confirmar) y Atrás (volver).
class GamesActivity final : public Activity {
 public:
  explicit GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Games", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int selected = 0;
  ButtonNavigator buttonNavigator;
};
