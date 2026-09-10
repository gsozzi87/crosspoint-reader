#include "Game2048Activity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_random.h>

#include <algorithm>

#include "GameUi.h"
#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "input/MotionInput.h"

namespace {
constexpr int SIDE = gameui::SIDE;
constexpr int PARTIALS_BEFORE_CLEAN = 10;
constexpr unsigned long BACK_HOLD_MS = 1000;
}  // namespace

void Game2048Activity::onEnter() {
  Activity::onEnter();
  useMotion = MOTION.available() && HUB_STORE.motionGestures;
  reset();
  requestUpdate();
}

void Game2048Activity::reset() {
  grid.fill(0);
  score = 0;
  over = false;
  aim = Dir::Left;
  spawn();
  spawn();
  forceClean = true;
}

void Game2048Activity::spawn() {
  int free[N * N];
  int n = 0;
  for (int i = 0; i < N * N; ++i) {
    if (!grid[i]) free[n++] = i;
  }
  if (!n) return;
  // Nueve de cada diez fichas nuevas son un 2, como en el original.
  grid[free[esp_random() % static_cast<uint32_t>(n)]] = (esp_random() % 10 == 0) ? 4 : 2;
}

// Empuja todo en la dirección pedida y junta pares iguales. Cada ficha se
// fusiona una sola vez por jugada (si no, una fila de cuatro doses daría un
// ocho en vez de dos cuatros).
bool Game2048Activity::slide(const Dir d) {
  bool moved = false;
  for (int line = 0; line < N; ++line) {
    uint16_t v[N];
    // Se lee la fila o la columna SIEMPRE en el sentido del empuje, así el
    // resto del algoritmo es uno solo.
    for (int i = 0; i < N; ++i) {
      int x = 0, y = 0;
      switch (d) {
        case Dir::Left: x = i; y = line; break;
        case Dir::Right: x = N - 1 - i; y = line; break;
        case Dir::Up: x = line; y = i; break;
        case Dir::Down: x = line; y = N - 1 - i; break;
      }
      v[i] = grid[static_cast<size_t>(y) * N + x];
    }

    uint16_t out[N] = {0, 0, 0, 0};
    int w = 0;
    for (int i = 0; i < N; ++i) {
      if (!v[i]) continue;
      if (w > 0 && out[w - 1] == v[i]) {
        out[w - 1] = static_cast<uint16_t>(out[w - 1] * 2);
        score += out[w - 1];
      } else {
        out[w++] = v[i];
      }
    }

    for (int i = 0; i < N; ++i) {
      if (out[i] == v[i]) continue;
      moved = true;
      break;
    }
    for (int i = 0; i < N; ++i) {
      int x = 0, y = 0;
      switch (d) {
        case Dir::Left: x = i; y = line; break;
        case Dir::Right: x = N - 1 - i; y = line; break;
        case Dir::Up: x = line; y = i; break;
        case Dir::Down: x = line; y = N - 1 - i; break;
      }
      grid[static_cast<size_t>(y) * N + x] = out[i];
    }
  }
  return moved;
}

bool Game2048Activity::movesLeft() const {
  for (int i = 0; i < N * N; ++i) {
    if (!grid[i]) return true;
  }
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      const uint16_t v = grid[static_cast<size_t>(y) * N + x];
      if (x + 1 < N && grid[static_cast<size_t>(y) * N + x + 1] == v) return true;
      if (y + 1 < N && grid[static_cast<size_t>(y + 1) * N + x] == v) return true;
    }
  }
  return false;
}

void Game2048Activity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    if (!backHeldSince) backHeldSince = millis();
    else if (millis() - backHeldSince >= BACK_HOLD_MS) {
      backHeldSince = 0;
      reset();
      requestUpdate();
    }
    return;
  }
  if (backHeldSince) {
    backHeldSince = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
  }

  if (over) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      reset();
      requestUpdate();
    }
    return;
  }

  Dir dir = aim;
  bool play = false;
  if (useMotion) {
    if (MOTION.take(MotionInput::Event::TiltLeft)) { dir = Dir::Left; play = true; }
    else if (MOTION.take(MotionInput::Event::TiltRight)) { dir = Dir::Right; play = true; }
    else if (MOTION.take(MotionInput::Event::TiltForward)) { dir = Dir::Down; play = true; }
    else if (MOTION.take(MotionInput::Event::TiltBack)) { dir = Dir::Up; play = true; }
  }
  if (!play) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      aim = static_cast<Dir>((static_cast<int>(aim) + 3) % 4);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      aim = static_cast<Dir>((static_cast<int>(aim) + 1) % 4);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      dir = aim;
      play = true;
    }
  }
  if (!play) return;

  if (!slide(dir)) return;  // nada se movió: no es jugada, no nace ficha
  spawn();
  if (score > best) best = score;
  if (!movesLeft()) over = true;
  requestUpdate();
}

void Game2048Activity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + 2 * gameui::GAP;
  const int screenBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  helpTop = screenBottom - gameui::helpHeight(renderer);
  statsTop = helpTop - gameui::statsHeight(renderer);
  statusTop = statsTop - gameui::GAP - gameui::statusHeight(renderer);

  const int bottom = statusTop - gameui::GAP;
  const int avail = std::min(pageWidth - 2 * SIDE, bottom - top);
  cell = std::max(24, avail / N);
  originX = (pageWidth - cell * N) / 2;
  originY = top + ((bottom - top) - cell * N) / 2;
}

void Game2048Activity::drawBoard() {
  renderer.drawRect(originX - 3, originY - 3, cell * N + 6, cell * N + 6, 2, true);
  char text[8];
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      const int px = originX + x * cell;
      const int py = originY + y * cell;
      const uint16_t v = grid[static_cast<size_t>(y) * N + x];
      renderer.drawRect(px + 2, py + 2, cell - 4, cell - 4, true);
      if (!v) continue;
      // Sin color, el "cuánto vale" lo dice la trama: los chicos van sobre
      // blanco, del 32 para arriba sobre gris claro y del 256 sobre gris
      // oscuro. Nunca negro macizo: el número no se leería.
      if (v >= 256) renderer.fillRectDither(px + 4, py + 4, cell - 8, cell - 8, DarkGray);
      else if (v >= 32) renderer.fillRectDither(px + 4, py + 4, cell - 8, cell - 8, LightGray);
      snprintf(text, sizeof(text), "%u", static_cast<unsigned>(v));
      // Los números de cuatro cifras no entran en el cuerpo grande.
      const int font = v >= 1024 ? UI_10_FONT_ID : UI_12_FONT_ID;
      const int tw = renderer.getTextWidth(font, text, EpdFontFamily::BOLD);
      const int th = renderer.getLineHeight(font);
      const int tx = px + (cell - tw) / 2;
      const int ty = py + (cell - th) / 2;
      // Plato blanco debajo del número: la regla del rediseño es que NUNCA hay
      // letras sobre trama, y del 32 para arriba la celda está tramada.
      if (v >= 32) drawTextPlate(renderer, tx - 6, ty - 2, tw + 12, th + 4);
      renderer.drawText(font, tx, ty, text, true, EpdFontFamily::BOLD);
    }
  }
}

// La flecha del empuje, igual que en el laberinto.
void Game2048Activity::drawAim(const int cx, const int cy) const {
  const int r = 14;
  int tipX = cx, tipY = cy, baseX = cx, baseY = cy;
  switch (aim) {
    case Dir::Up: tipY = cy - r; baseY = cy + r; break;
    case Dir::Down: tipY = cy + r; baseY = cy - r; break;
    case Dir::Left: tipX = cx - r; baseX = cx + r; break;
    case Dir::Right: tipX = cx + r; baseX = cx - r; break;
  }
  renderer.drawLine(baseX, baseY, tipX, tipY, 2, true);
  const int wx = (aim == Dir::Up || aim == Dir::Down) ? 7 : 0;
  const int wy = (aim == Dir::Up || aim == Dir::Down) ? 0 : 7;
  const int backX = tipX + (baseX - tipX) / 2;
  const int backY = tipY + (baseY - tipY) / 2;
  renderer.drawLine(tipX, tipY, backX + wx, backY + wy, 2, true);
  renderer.drawLine(tipX, tipY, backX - wx, backY - wy, 2, true);
}

void Game2048Activity::drawInfo() {
  const int contentW = gameui::contentWidth(renderer);

  gameui::status(renderer, SIDE, statusTop, contentW, over ? tr(STR_2048_OVER) : tr(STR_GAME_YOUR_TURN), nullptr);
  if (!over) drawAim(SIDE + contentW - 20, statusTop + gameui::statusHeight(renderer) / 2);

  // El puntaje en UI_14: es el dato que se mira entre jugada y jugada.
  char vScore[12], vBest[12];
  snprintf(vScore, sizeof(vScore), "%d", score);
  snprintf(vBest, sizeof(vBest), "%d", best);
  const gameui::Stat scoreboard[2] = {{vScore, tr(STR_GAME_SCORE)}, {vBest, tr(STR_GAME_BEST)}};
  gameui::stats(renderer, SIDE, statsTop, contentW, scoreboard, 2);

  // Terminada la partida el loop() ni mira la palanca, así que anunciar el
  // control de movimiento sería mentir: queda sólo lo que SÍ se puede hacer.
  // Y los dos pedazos van unidos por " · ", como el resto de la línea, no por
  // un espacio pelado.
  char helpText[192];
  if (over) {
    snprintf(helpText, sizeof(helpText), "%s", tr(STR_GAME_AGAIN));
  } else {
    snprintf(helpText, sizeof(helpText), "%s · %s", useMotion ? tr(STR_2048_HINT) : tr(STR_2048_HINT_LEVER),
             tr(STR_GAME_HELP_RESTART));
  }
  gameui::help(renderer, helpTop, helpText);
}

void Game2048Activity::render(RenderLock&&) {
  layout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_2048));
  drawBoard();
  drawInfo();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
