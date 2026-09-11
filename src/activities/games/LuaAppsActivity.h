#pragma once

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "lua/LuaApp.h"
#include "util/ButtonNavigator.h"

// El catálogo de apps de la tarjeta y el sitio donde corren.
//
// Dos estados en una sola Activity a propósito: entrar y salir de una app es un
// cambio de estado, no un empujón de pantalla, así el intérprete se abre y se
// cierra en un solo lugar y no queda vivo si alguien navega de otra forma.
//
// Atrás se le pasa a la app: si lo atiende (devuelve true) se sigue adentro, y
// si no, se vuelve al catálogo. Atrás MANTENIDO sale siempre, así una app que
// se coma el botón no puede dejar al usuario encerrado.
class LuaAppsActivity final : public Activity {
 public:
  explicit LuaAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LuaApps", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State : uint8_t { LIST, RUNNING, FAILED };

  void startSelected();
  void backToList();
  void renderList();
  void renderError();
  int visibleRows() const;
  void clampScroll();

  State state = LIST;
  std::vector<LuaApp::Entry> apps;
  int selected = 0;
  int scroll = 0;
  std::unique_ptr<LuaApp> app;
  unsigned long lastTick = 0;
  ButtonNavigator buttonNavigator;
};
