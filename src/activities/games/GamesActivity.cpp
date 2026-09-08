#include "GamesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BlackjackActivity.h"
#include "CheckersActivity.h"
#include "MappedInputManager.h"
#include "MathActivity.h"
#include "MemoryActivity.h"
#include "SimonActivity.h"
#include "SudokuActivity.h"
#include "TetrisActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ROW_H = 56;
constexpr int SIDE = 20;

struct GameSpec {
  StrId name;
  StrId hint;
};

// Orden del menú. El texto de abajo dice de qué se trata en una línea.
const GameSpec GAMES[] = {
    {StrId::STR_GAME_CHECKERS, StrId::STR_GAME_CHECKERS_DESC},
    {StrId::STR_GAME_SUDOKU, StrId::STR_GAME_SUDOKU_DESC},
    {StrId::STR_GAME_BLACKJACK, StrId::STR_GAME_BLACKJACK_DESC},
    {StrId::STR_GAME_MEMORY, StrId::STR_GAME_MEMORY_DESC},
    {StrId::STR_GAME_SIMON, StrId::STR_GAME_SIMON_DESC},
    {StrId::STR_GAME_MATH, StrId::STR_GAME_MATH_DESC},
    {StrId::STR_GAME_TETRIS, StrId::STR_GAME_TETRIS_DESC},
};
constexpr int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);
}  // namespace

void GamesActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void GamesActivity::loop() {
  buttonNavigator.onNext([this] {
    selected = ButtonNavigator::nextIndex(selected, GAME_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = ButtonNavigator::previousIndex(selected, GAME_COUNT);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (selected) {
      case 0: startActivityForResult(std::make_unique<CheckersActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 1: startActivityForResult(std::make_unique<SudokuActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 2: startActivityForResult(std::make_unique<BlackjackActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 3: startActivityForResult(std::make_unique<MemoryActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 4: startActivityForResult(std::make_unique<SimonActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 5: startActivityForResult(std::make_unique<MathActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      case 6: startActivityForResult(std::make_unique<TetrisActivity>(renderer, mappedInput), [this](const ActivityResult&) { requestUpdate(); }); break;
      default: break;
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) activityManager.goHome();
}

void GamesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_GAMES_TITLE));

  const int top = metrics.topPadding + metrics.headerHeight + 14;
  for (int i = 0; i < GAME_COUNT; ++i) {
    const int y = top + i * ROW_H;
    const bool sel = i == selected;
    if (sel) renderer.fillRoundedRect(SIDE - 8, y, pageWidth - 2 * (SIDE - 8), ROW_H - 8, 10, Color::Black);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 8, I18N.get(GAMES[i].name), !sel, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, SIDE, y + 30, I18N.get(GAMES[i].hint), !sel);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
