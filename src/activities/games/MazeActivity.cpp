#include "MazeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_random.h>

#include <algorithm>

#include "GameUi.h"
#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "input/MotionInput.h"

namespace {
constexpr int SIDE = gameui::SIDE;
constexpr int PARTIALS_BEFORE_CLEAN = 10;
constexpr unsigned long BACK_HOLD_MS = 1000;
constexpr int MAX_COLS = 15;
constexpr int MAX_ROWS = 25;

int dx(const int d) { return d == 1 ? 1 : d == 3 ? -1 : 0; }
int dy(const int d) { return d == 2 ? 1 : d == 0 ? -1 : 0; }
}  // namespace

void MazeActivity::onEnter() {
  Activity::onEnter();
  level = 1;
  useMotion = MOTION.available() && HUB_STORE.motionGestures;
  generate();
  requestUpdate();
}

void MazeActivity::onExit() { Activity::onExit(); }

// Laberinto perfecto por camino aleatorio con vuelta atrás (el clásico "recursive
// backtracker", iterativo para no comerse la pila): siempre hay exactamente un
// camino entre dos celdas, así que nunca sale uno imposible.
void MazeActivity::generate() {
  cols = std::min(MAX_COLS, 7 + level);
  rows = std::min(MAX_ROWS, 11 + level * 2);
  cells.assign(static_cast<size_t>(cols) * rows, UP_W | RIGHT_W | DOWN_W | LEFT_W);

  std::vector<uint8_t> seen(static_cast<size_t>(cols) * rows, 0);
  std::vector<int> stack;
  stack.reserve(static_cast<size_t>(cols) * rows);
  int cx = 0, cy = 0;
  seen[0] = 1;
  stack.push_back(0);
  while (!stack.empty()) {
    const int here = stack.back();
    cx = here % cols;
    cy = here / cols;
    int options[4];
    int n = 0;
    for (int d = 0; d < 4; ++d) {
      const int nx = cx + dx(d), ny = cy + dy(d);
      if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
      if (seen[static_cast<size_t>(ny) * cols + nx]) continue;
      options[n++] = d;
    }
    if (!n) {
      stack.pop_back();
      continue;
    }
    const int d = options[esp_random() % static_cast<uint32_t>(n)];
    const int nx = cx + dx(d), ny = cy + dy(d);
    // Tirar la pared de los dos lados a la vez.
    static const uint8_t OPEN[4] = {UP_W, RIGHT_W, DOWN_W, LEFT_W};
    static const uint8_t BACK[4] = {DOWN_W, LEFT_W, UP_W, RIGHT_W};
    cells[static_cast<size_t>(cy) * cols + cx] &= static_cast<uint8_t>(~OPEN[d]);
    cells[static_cast<size_t>(ny) * cols + nx] &= static_cast<uint8_t>(~BACK[d]);
    seen[static_cast<size_t>(ny) * cols + nx] = 1;
    stack.push_back(ny * cols + nx);
  }

  ballX = 0;
  ballY = 0;
  goalX = cols - 1;
  goalY = rows - 1;
  moves = 0;
  won = false;
  aim = Dir::Right;
  forceClean = true;
}

bool MazeActivity::canMove(const int cx, const int cy, const Dir d) const {
  static const uint8_t MASK[4] = {UP_W, RIGHT_W, DOWN_W, LEFT_W};
  return (cells[static_cast<size_t>(cy) * cols + cx] & MASK[static_cast<int>(d)]) == 0;
}

// Una tirada: la bolita rueda hasta chocar. Se para antes si pasa por la salida
// o si el pasillo se abre a un costado (si no, en un laberinto largo una sola
// tirada te cruza media pantalla y el juego se juega solo).
void MazeActivity::roll(const Dir d) {
  const int step = static_cast<int>(d);
  int steps = 0;
  while (canMove(ballX, ballY, d)) {
    ballX += dx(step);
    ballY += dy(step);
    ++steps;
    if (ballX == goalX && ballY == goalY) break;
    // ¿Hay una bifurcación acá? Entonces la bolita se para y decide el jugador.
    int exits = 0;
    for (int k = 0; k < 4; ++k) {
      if (canMove(ballX, ballY, static_cast<Dir>(k))) ++exits;
    }
    if (exits > 2) break;
  }
  if (!steps) return;  // pared: no cuenta como jugada
  ++moves;
  if (ballX == goalX && ballY == goalY) won = true;
  requestUpdate();
}

void MazeActivity::loop() {
  // Atrás: toque sale, mantenido empieza otro.
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    if (!backHeldSince) backHeldSince = millis();
    else if (millis() - backHeldSince >= BACK_HOLD_MS) {
      backHeldSince = 0;
      generate();
      requestUpdate();
      return;
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

  if (won) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      ++level;
      generate();
      requestUpdate();
    }
    return;
  }

  if (useMotion) {
    if (MOTION.take(MotionInput::Event::TiltLeft)) roll(Dir::Left);
    else if (MOTION.take(MotionInput::Event::TiltRight)) roll(Dir::Right);
    else if (MOTION.take(MotionInput::Event::TiltForward)) roll(Dir::Down);
    else if (MOTION.take(MotionInput::Event::TiltBack)) roll(Dir::Up);
  }

  // La palanca gira la flecha y OK tira. Está siempre, también con el sensor
  // andando: es la forma de jugarlo sentado sin mover el aparato.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    aim = static_cast<Dir>((static_cast<int>(aim) + 3) % 4);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    aim = static_cast<Dir>((static_cast<int>(aim) + 1) % 4);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    roll(aim);
  }
}

void MazeActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + gameui::GAP;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  helpTop = bottom - gameui::helpHeight(renderer);
  statsTop = helpTop - gameui::statsHeight(renderer);
  statusTop = statsTop - gameui::GAP - gameui::statusHeight(renderer);

  const int availW = pageWidth - 2 * SIDE;
  const int availH = statusTop - gameui::GAP - top;
  cell = std::max(8, std::min(availW / cols, availH / rows));
  originX = (pageWidth - cell * cols) / 2;
  originY = top + (availH - cell * rows) / 2;
}

void MazeActivity::drawMaze() {
  // Marco exterior grueso: en tinta electrónica una línea de 1 px al borde de
  // un rectángulo grande se pierde.
  renderer.drawRect(originX - 2, originY - 2, cell * cols + 4, cell * rows + 4, true);

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const uint8_t w = cells[static_cast<size_t>(y) * cols + x];
      const int px = originX + x * cell;
      const int py = originY + y * cell;
      // Solo arriba y a la izquierda de cada celda, más el borde: si no, cada
      // pared se dibuja dos veces y queda del doble de gruesa.
      if (w & UP_W) renderer.drawLine(px, py, px + cell, py, true);
      if (w & LEFT_W) renderer.drawLine(px, py, px, py + cell, true);
      if (x == cols - 1 && (w & RIGHT_W)) renderer.drawLine(px + cell, py, px + cell, py + cell, true);
      if (y == rows - 1 && (w & DOWN_W)) renderer.drawLine(px, py + cell, px + cell, py + cell, true);
    }
  }

  // La salida: un cuadrado tramado, que se ve sin competir con la bolita.
  const int gx = originX + goalX * cell;
  const int gy = originY + goalY * cell;
  renderer.fillRectDither(gx + 3, gy + 3, cell - 6, cell - 6, LightGray);

  // La bolita: disco lleno, lo único negro macizo de la pantalla.
  const int r = std::max(3, cell / 2 - 4);
  const int bx = originX + ballX * cell + cell / 2;
  const int by = originY + ballY * cell + cell / 2;
  renderer.fillRoundedRect(bx - r, by - r, r * 2, r * 2, r, Black);
}

// La flecha de dirección, dibujada como flecha y no escrita: se entiende sin
// leer y sin saber el idioma.
void MazeActivity::drawAim(const int cx, const int cy) const {
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

void MazeActivity::drawInfo() {
  const int contentW = gameui::contentWidth(renderer);

  // Estado en UI_14: es el renglón que se mira entre tirada y tirada.
  gameui::status(renderer, SIDE, statusTop, contentW, won ? tr(STR_MAZE_WON) : tr(STR_GAME_YOUR_TURN), nullptr);
  if (!won) drawAim(SIDE + contentW - 20, statusTop + gameui::statusHeight(renderer) / 2);

  char vLevel[8], vMoves[8];
  snprintf(vLevel, sizeof(vLevel), "%d", level);
  snprintf(vMoves, sizeof(vMoves), "%d", moves);
  const gameui::Stat scoreboard[2] = {{vLevel, tr(STR_GAME_LEVEL)}, {vMoves, tr(STR_GAME_MOVES)}};
  gameui::stats(renderer, SIDE, statsTop, contentW, scoreboard, 2);

  // Terminada la partida el loop() ni mira la palanca, así que anunciar el
  // control de movimiento sería mentir: queda sólo lo que SÍ se puede hacer.
  // Y los dos pedazos van unidos por " · ", como el resto de la línea, no por
  // un espacio pelado.
  char helpText[192];
  if (won) {
    snprintf(helpText, sizeof(helpText), "%s", tr(STR_GAME_AGAIN));
  } else {
    snprintf(helpText, sizeof(helpText), "%s · %s", useMotion ? tr(STR_MAZE_HINT) : tr(STR_2048_HINT_LEVER),
             tr(STR_GAME_HELP_RESTART));
  }
  gameui::help(renderer, helpTop, helpText);
}

void MazeActivity::render(RenderLock&&) {
  layout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_MAZE));
  drawMaze();
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
