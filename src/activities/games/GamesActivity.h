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
  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 12 parciales

  int selected = 0;
  int partialCount = 0;
  bool forceClean = true;
  ButtonNavigator buttonNavigator;
};
