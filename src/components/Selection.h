#pragma once

#include <GfxRenderer.h>

// El resalte de lo que está elegido.
//
// Hasta 1.5.43 la fila (o el mosaico) elegido se pintaba de NEGRO macizo y el
// texto se invertía a blanco. En el papel electrónico eso es un manchón: pega
// un salto de contraste enorme, deja fantasma en el refresco parcial siguiente
// y el usuario lo pidió expresamente — "lo que selecciono tanto en el hub como
// en todos lados, no quiero que sea negro, mejor un gris o algo no tan
// contrastante".
//
// 1.5.48 arregla el defecto que quedó de eso: la trama tapaba TODA la fila, o
// sea que el renglón sobre el que uno va a apretar OK era el MENOS legible de
// la pantalla ("De lunes a viernes" sobre puntos gris se ensucia). Ahora el
// resalte tiene tres partes y ninguna cae sobre el texto:
//
//   1. una PESTAÑA negra de 5 px pegada al borde izquierdo. Es el único aviso
//      que se sigue viendo cuando la trama se lava después de diez refrescos
//      parciales, y a un metro se ve sin buscarlo;
//   2. un MARCO fino alrededor de toda la fila;
//   3. dos FRANJAS tramadas en los márgenes (donde no hay letras), que dan el
//      peso de "esto está elegido" sin ensuciar nada.
//
// El centro queda BLANCO y el texto, negro: la fila elegida se lee igual de
// bien que las demás. Los mosaicos del hub usan el estilo Tile, que no lleva
// pestaña (la celda es cuadrada y la pestaña queda torcida) y trama el campo
// del ícono en vez de los márgenes.
constexpr Color SELECTION_FILL = Color::LightGray;
constexpr bool SELECTION_INK = true;

// Ancho de la pestaña y de las franjas tramadas de los costados.
constexpr int SELECTION_TAB_W = 5;
constexpr int SELECTION_BAND_W = 16;

enum class SelectionStyle : uint8_t {
  Row,   // filas de lista: pestaña + marco + franjas al costado
  Tile,  // mosaicos del hub y cuadrículas: marco + trama, sin pestaña
};

inline void drawSelectionRow(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                             const int radius = 8, const SelectionStyle style = SelectionStyle::Row) {
  if (style == SelectionStyle::Tile) {
    renderer.fillRoundedRect(x, y, w, h, radius, SELECTION_FILL);
    renderer.drawRoundedRect(x, y, w, h, 2, radius, true);
    return;
  }

  // Marco de toda la fila.
  renderer.drawRoundedRect(x, y, w, h, 2, radius, true);
  // Franjas tramadas a los costados, por dentro del marco. Se quedan cortas a
  // propósito en las esquinas redondeadas (radius/2) para no pisarlas.
  const int inset = 3;
  const int bandY = y + inset + radius / 2;
  const int bandH = h - 2 * (inset + radius / 2);
  if (bandH > 0) {
    const int band = w > 4 * SELECTION_BAND_W ? SELECTION_BAND_W : w / 6;
    if (band > 0) {
      renderer.fillRectDither(x + inset, bandY, band, bandH, SELECTION_FILL);
      renderer.fillRectDither(x + w - inset - band, bandY, band, bandH, SELECTION_FILL);
    }
  }
  // Pestaña: lo único macizo, y es de 5 px, así que no fantasmea.
  const int tabY = y + 4;
  const int tabH = h - 8;
  if (tabH > 0) renderer.fillRect(x + 1, tabY, SELECTION_TAB_W, tabH, true);
}

// Plato blanco detrás de un texto que cae sobre algo tramado (el campo del
// ícono de un mosaico, una celda de calendario, un tablero). Regla del diseño:
// NUNCA hay letras sobre trama.
inline void drawTextPlate(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  renderer.fillRect(x, y, w, h, false);
}
