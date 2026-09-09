#include "GamesActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>

#include "BlackjackActivity.h"
#include "CardsActivity.h"
#include "CheckersActivity.h"
#include "ChessActivity.h"
#include "ConnectFourActivity.h"
#include "MappedInputManager.h"
#include "MathActivity.h"
#include "MemoryActivity.h"
#include "RummyActivity.h"
#include "SudokuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/Selection.h"

namespace {
constexpr int ROW_H = 52;
constexpr int SIDE = 20;

using Factory = std::unique_ptr<Activity> (*)(GfxRenderer&, MappedInputManager&);

// Una plantilla en vez de doce lambdas iguales: la tabla de abajo es toda la
// lista de juegos, y agregar uno es una línea.
template <typename T>
std::unique_ptr<Activity> make(GfxRenderer& renderer, MappedInputManager& input) {
  return std::make_unique<T>(renderer, input);
}

struct GameSpec {
  StrId name;
  StrId hint;
  Factory create;
};

// Solo juegos POR TURNOS. Los de accion (viborita, ladrillos, bloques) se
// sacaron en 1.5.41: un refresco parcial de este panel tarda ~30 ms y ademas hay
// que hacer uno completo cada 12, asi que no hay forma de mover algo de corrido
// sin que parpadee y vaya a tirones. En un juego por turnos ese mismo refresco
// por jugada no molesta.
const GameSpec GAMES[] = {
    {StrId::STR_GAME_MEMORY, StrId::STR_GAME_MEMORY_DESC, &make<MemoryActivity>},
    {StrId::STR_GAME_MATH, StrId::STR_GAME_MATH_DESC, &make<MathActivity>},
    {StrId::STR_GAME_SUDOKU, StrId::STR_GAME_SUDOKU_DESC, &make<SudokuActivity>},
    {StrId::STR_GAME_CONNECT4, StrId::STR_GAME_CONNECT4_DESC, &make<ConnectFourActivity>},
    {StrId::STR_GAME_CHECKERS, StrId::STR_GAME_CHECKERS_DESC, &make<CheckersActivity>},
    {StrId::STR_GAME_CHESS, StrId::STR_GAME_CHESS_DESC, &make<ChessActivity>},
    {StrId::STR_GAME_BLACKJACK, StrId::STR_GAME_BLACKJACK_DESC, &make<BlackjackActivity>},
    {StrId::STR_GAME_RUMMY, StrId::STR_GAME_RUMMY_DESC, &make<RummyActivity>},
    {StrId::STR_GAME_CARDS, StrId::STR_GAME_CARDS_DESC, &make<CardsActivity>},
};
constexpr int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);
}  // namespace

void GamesActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

// Cuántas filas entran de verdad entre el encabezado y la barra de botones. Se
// calcula con las métricas del tema porque cada uno tiene su alto de barra.
int GamesActivity::visibleRows() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + 14;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, (bottom - top) / ROW_H);
}

// La ventana sigue a la selección: solo se mueve cuando el elegido se saldría.
void GamesActivity::clampScroll() {
  const int rows = visibleRows();
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + rows) scroll = selected - rows + 1;
  scroll = std::max(0, std::min(scroll, std::max(0, GAME_COUNT - rows)));
}

void GamesActivity::loop() {
  buttonNavigator.onNext([this] {
    selected = ButtonNavigator::nextIndex(selected, GAME_COUNT);
    clampScroll();
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = ButtonNavigator::previousIndex(selected, GAME_COUNT);
    clampScroll();
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startActivityForResult(GAMES[selected].create(renderer, mappedInput), [this](const ActivityResult&) {
      forceClean = true;
      requestUpdate();
    });
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) activityManager.goHome();
}

void GamesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_GAMES_TITLE));

  const int rows = visibleRows();
  const int top = metrics.topPadding + metrics.headerHeight + 14;
  const int textW = pageWidth - 2 * SIDE - 12;
  for (int row = 0; row < rows && scroll + row < GAME_COUNT; ++row) {
    const int i = scroll + row;
    const int y = top + row * ROW_H;
    const bool sel = i == selected;
    if (sel) drawSelectionRow(renderer, SIDE - 8, y, pageWidth - 2 * (SIDE - 8), ROW_H - 8, 10);
    const char* name = I18N.get(GAMES[i].name);
    const char* hint = I18N.get(GAMES[i].hint);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 6,
                      renderer.truncatedText(UI_12_FONT_ID, name, textW, EpdFontFamily::BOLD).c_str(), SELECTION_INK,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, SIDE, y + 28,
                      renderer.truncatedText(SMALL_FONT_ID, hint, textW).c_str(), SELECTION_INK);
  }

  // Solo cuando hay más juegos de los que entran: dice cuál de cuántos es.
  if (GAME_COUNT > rows) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%d/%d", selected + 1, GAME_COUNT);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, pager);
    renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - w, metrics.topPadding + metrics.headerHeight - 6, pager);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Regla del panel: parcial rápido al mover la selección y uno limpio cada
  // doce (y al entrar), que si no la lista queda fantasmeada.
  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
