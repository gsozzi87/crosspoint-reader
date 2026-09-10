#include "LuaAppsActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ROW_H = 52;
constexpr int SIDE = 24;      // ÚNICO margen lateral de la pantalla
constexpr int TEXT_PAD = 24;  // borde de la fila -> texto: por dentro de la franja del resalte
// Atrás mantenido sale siempre, aunque la app se coma el botón.
constexpr unsigned long EXIT_HOLD_MS = 1000;
}  // namespace

void LuaAppsActivity::onEnter() {
  Activity::onEnter();
  apps = LuaApp::installed();
  selected = 0;
  scroll = 0;
  state = LIST;
  requestUpdate();
}

void LuaAppsActivity::onExit() {
  // El intérprete se cierra acá pase lo que pase: es lo que garantiza que no
  // quede una app viva si se sale por un camino raro (una alarma, el hub).
  app.reset();
  Activity::onExit();
}

int LuaAppsActivity::visibleRows() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + 14;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, (bottom - top) / ROW_H);
}

void LuaAppsActivity::clampScroll() {
  const int count = static_cast<int>(apps.size());
  const int rows = visibleRows();
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + rows) scroll = selected - rows + 1;
  scroll = std::max(0, std::min(scroll, std::max(0, count - rows)));
}

void LuaAppsActivity::startSelected() {
  if (apps.empty()) return;
  app = std::make_unique<LuaApp>();
  if (!app->open(renderer, apps[selected].path)) {
    state = FAILED;
  } else {
    state = RUNNING;
    lastTick = millis();
  }
  requestUpdate();
}

void LuaAppsActivity::backToList() {
  app.reset();
  state = LIST;
  requestUpdate();
}

void LuaAppsActivity::loop() {
  if (state == LIST) {
    buttonNavigator.onNext([this] {
      if (apps.empty()) return;
      selected = ButtonNavigator::nextIndex(selected, static_cast<int>(apps.size()));
      clampScroll();
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      if (apps.empty()) return;
      selected = ButtonNavigator::previousIndex(selected, static_cast<int>(apps.size()));
      clampScroll();
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      backToList();
    }
    return;
  }

  // --- Corriendo ----------------------------------------------------------
  if (!app || !app->ok()) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // La salida de emergencia va PRIMERO: si la app se cuelga con el botón, esto
  // sigue funcionando.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, EXIT_HOLD_MS)) {
    backToList();
    return;
  }

  const char* key = nullptr;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) key = "up";
  else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) key = "down";
  else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) key = "ok";
  else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) key = "back";

  if (key) {
    const bool repaint = app->onKey(key);
    // Atrás que la app no atendió = salir. Es lo que evita que una app sin
    // `on_key` deje al usuario adentro sin saber cómo volver.
    if (strcmp(key, "back") == 0 && !repaint && !app->quitRequested() && app->ok()) {
      backToList();
      return;
    }
    if (repaint) requestUpdate();
  } else if (millis() - lastTick >= LuaApp::TICK_MS) {
    lastTick = millis();
    if (app->onTick()) requestUpdate();
  }

  if (app->quitRequested()) {
    backToList();
    return;
  }
  if (!app->ok()) {
    state = FAILED;
    requestUpdate();
  }
}

void LuaAppsActivity::renderList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LUA_APPS));

  if (apps.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20, tr(STR_LUA_NONE), true);
    renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12, LuaApp::dir(), true);
  } else {
    const int rows = visibleRows();
    const int top = metrics.topPadding + metrics.headerHeight + 14;
    const int rowW = pageWidth - 2 * SIDE;
    const int textX = SIDE + TEXT_PAD;
    const int textW = rowW - 2 * TEXT_PAD;
    for (int row = 0; row < rows && scroll + row < static_cast<int>(apps.size()); ++row) {
      const int i = scroll + row;
      const int y = top + row * ROW_H;
      if (i == selected) drawSelectionRow(renderer, SIDE, y, rowW, ROW_H - 8, 10);
      renderer.drawText(UI_12_FONT_ID, textX, y + 6,
                        renderer.truncatedText(UI_12_FONT_ID, apps[i].name.c_str(), textW, EpdFontFamily::BOLD).c_str(),
                        SELECTION_INK, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, textX, y + 28,
                        renderer.truncatedText(SMALL_FONT_ID, apps[i].path.c_str(), textW).c_str(), SELECTION_INK);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), apps.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::renderError() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LUA_ERROR));

  int y = metrics.topPadding + metrics.headerHeight + 32;
  if (app) {
    renderer.drawText(UI_12_FONT_ID, SIDE, y, app->name().c_str(), true, EpdFontFamily::BOLD);
    y += 40;
    // El mensaje del intérprete tal cual: es lo único que le dice al que
    // escribió la app dónde está el problema, y viene con archivo y línea.
    std::string rest = app->error();
    const int textW = pageWidth - 2 * SIDE;
    while (!rest.empty() && y < renderer.getScreenHeight() - metrics.buttonHintsHeight - 40) {
      const std::string shown = renderer.truncatedText(SMALL_FONT_ID, rest.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, SIDE, y, shown.c_str());
      y += 22;
      const size_t cut = shown.size() >= 3 ? shown.size() - 3 : shown.size();
      if (shown.size() < rest.size() && cut > 0 && cut < rest.size()) {
        rest = rest.substr(cut);
      } else {
        break;
      }
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::render(RenderLock&&) {
  if (state == RUNNING && app && app->ok()) {
    // La pantalla se le da limpia a la app y el refresco lo decide el
    // firmware: una app no elige cuándo se refresca el panel.
    renderer.clearScreen();
    app->onDraw();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (state == FAILED) {
    renderError();
  } else {
    renderList();
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
