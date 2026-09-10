#include "GameUi.h"

#include <EpdFontFamily.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fontIds.h"

namespace gameui {

void rule(const GfxRenderer& renderer, const int x, const int y, const int w) {
  renderer.fillRect(x, y, w, 1, true);
}

int contentWidth(const GfxRenderer& renderer) { return renderer.getScreenWidth() - 2 * SIDE; }

// ------------------------------------------------------------------ ayuda ---

int helpHeight(const GfxRenderer& renderer) {
  return GAP + HELP_LINES * renderer.getLineHeight(UI_10_FONT_ID);
}

void help(const GfxRenderer& renderer, const int y, const char* text) {
  if (!text || !*text) return;
  const int w = contentWidth(renderer);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, text, w, HELP_LINES);
  for (size_t i = 0; i < lines.size(); ++i) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + GAP + static_cast<int>(i) * lineHeight, lines[i].c_str());
  }
}

// ----------------------------------------------------------------- estado ---

int statusHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(UI_14_FONT_ID); }

// UI_14 se pide en REGULAR a propósito: la familia se armó con una sola cara (la
// negrita), así que REGULAR da el mismo glifo sin pagar la búsqueda de una cara
// que no existe.
void status(const GfxRenderer& renderer, const int x, const int y, const int w, const char* title,
            const char* detail) {
  const bool hasTitle = title && *title;
  const bool hasDetail = detail && *detail;

  int detailW = 0;
  std::string shown;
  if (hasDetail) {
    // Se MIDE el título antes de repartir. El detalle termina en el contador
    // ("3 de 12"), que es lo único que dice si la palanca movió el cursor, así
    // que se queda con todo lo que el título no usa; el reparto por mitades
    // queda sólo para cuando el título tampoco entra, y ahí se corta el detalle
    // porque el estado es lo que identifica la pantalla.
    int budget = w;
    if (hasTitle) {
      const int titleW = renderer.getTextWidth(UI_14_FONT_ID, title);
      budget = std::max(w / 2, w - 2 * GAP - titleW);
    }
    shown = renderer.truncatedText(UI_10_FONT_ID, detail, budget);
    detailW = renderer.getTextWidth(UI_10_FONT_ID, shown.c_str());
  }
  if (hasTitle) {
    const int titleW = w - (detailW > 0 ? detailW + 2 * GAP : 0);
    renderer.drawText(UI_14_FONT_ID, x, y, renderer.truncatedText(UI_14_FONT_ID, title, titleW).c_str());
  }
  if (detailW > 0) {
    const int baseY =
        y + renderer.getFontAscenderSize(UI_14_FONT_ID) - renderer.getFontAscenderSize(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, x + w - detailW, baseY, shown.c_str());
  }
}

// ------------------------------------------------------------- marcadores ---

int statsHeight(const GfxRenderer& renderer, const int labelLines) {
  const int lines = labelLines > 0 ? labelLines : 1;
  return 1 + GAP + renderer.getLineHeight(UI_14_FONT_ID) + lines * renderer.getLineHeight(SMALL_FONT_ID);
}

void stats(const GfxRenderer& renderer, const int x, const int y, const int w, const Stat* items,
           const int count, const int labelLines) {
  if (!items || count <= 0 || w <= 0) return;
  rule(renderer, x, y, w);
  const int lines = labelLines > 0 ? labelLines : 1;
  const int valueLine = renderer.getLineHeight(UI_14_FONT_ID);
  const int labelLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int valueY = y + 1 + GAP;
  const int labelY = valueY + valueLine;

  // El ancho se reparte por peso y por acumulado (no `i * colW`): así el
  // redondeo no deja un hueco al final ni corre las reglas verticales.
  int totalWeight = 0;
  for (int i = 0; i < count; ++i) totalWeight += items[i].weight > 0 ? items[i].weight : 1;
  if (totalWeight <= 0) return;

  int acc = 0;
  for (int i = 0; i < count; ++i) {
    const int weight = items[i].weight > 0 ? items[i].weight : 1;
    const int colX = x + w * acc / totalWeight;
    acc += weight;
    const int colW = x + w * acc / totalWeight - colX;
    if (colW <= GAP) continue;
    // La regla vertical se queda corta arriba y abajo: llegar hasta el borde
    // arma una caja, y acá no queremos cajas.
    if (i > 0) renderer.fillRect(colX, valueY + 2, 1, valueLine + lines * labelLine - 8, true);

    const std::string value = renderer.truncatedText(UI_14_FONT_ID, items[i].value, colW - GAP);
    const int valueW = renderer.getTextWidth(UI_14_FONT_ID, value.c_str());
    renderer.drawText(UI_14_FONT_ID, colX + (colW - valueW) / 2, valueY, value.c_str());

    // Con más de un renglón reservado la etiqueta se PARTE en vez de salir con
    // puntos suspensivos: en cuatro columnas "ваши фигуры" no entra en 100 px.
    const auto labelRows = renderer.wrappedText(SMALL_FONT_ID, items[i].label, colW - GAP, lines);
    for (size_t r = 0; r < labelRows.size(); ++r) {
      const int labelW = renderer.getTextWidth(SMALL_FONT_ID, labelRows[r].c_str());
      renderer.drawText(SMALL_FONT_ID, colX + (colW - labelW) / 2, labelY + static_cast<int>(r) * labelLine,
                        labelRows[r].c_str());
    }
  }
}

// ---------------------------------------------------------------- tablero ---

void shadeCell(const GfxRenderer& renderer, const int x, const int y, const int cell) {
  renderer.fillRectDither(x, y, cell, cell, Color::LightGray);
}

void cursorFrame(const GfxRenderer& renderer, const int x, const int y, const int cell) {
  renderer.drawRect(x, y, cell, cell, CURSOR_W, true);
}

void lastMoveFrame(const GfxRenderer& renderer, const int x, const int y, const int cell) {
  renderer.drawRect(x + 2, y + 2, cell - 4, cell - 4, LAST_MOVE_W, true);
}

}  // namespace gameui
