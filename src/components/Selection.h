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
// Ahora el resalte es GRIS CLARO tramado (un píxel de tinta cada cuatro, ver
// GfxRenderer::fillRectImpl) y el texto de arriba SIGUE EN NEGRO: se lee igual
// de bien que el resto y la fila igual se distingue de un vistazo. Por eso
// SELECTION_INK es siempre `true`: donde antes iba `!sel` como color de texto,
// ahora va negro y punto.
constexpr Color SELECTION_FILL = Color::LightGray;
constexpr bool SELECTION_INK = true;

// Marco fino además del gris: en pantallas con mucho texto alrededor (mosaicos
// del hub, celdas del calendario) el gris solo se lee poco.
inline void drawSelectionRow(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                             const int radius = 8) {
  renderer.fillRoundedRect(x, y, w, h, radius, SELECTION_FILL);
  renderer.drawRoundedRect(x, y, w, h, 2, radius, true);
}
