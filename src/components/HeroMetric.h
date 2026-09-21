#pragma once

// EL NÚMERO HÉROE Y LA FILA DE MEDIDAS (REV-086).
//
// De las maquetas que mandó el dueño, esto es lo que de verdad valía la pena
// robar: un dato que manda en la pantalla (18°, 78 %) y, debajo, las medidas
// que lo acompañan en columnas con su etiqueta — no como un renglón de texto
// corrido separado por puntos, que es lo que había.
//
// Dos cosas que NO se copian de las maquetas, y están en `docs/ws397/DISENO.md`:
// no hay relleno negro (el negro grande deja fantasma en el parcial siguiente,
// ya se pagó una vez en 1.5.48) y no se copia la orientación vertical: el panel
// es 800x480 APAISADO y una columna de teléfono deja media pantalla vacía. Por
// eso las medidas van en columnas a todo el ancho, que es lo que el apaisado
// permite y el teléfono no.
//
// El número sale de `DISPLAY_32_FONT_ID`, una cara de sólo dígitos y seis
// símbolos (2,8 KB; la misma cara con el juego completo serían 157 KB). NO
// reemplaza a `SevenSegment`: los dígitos dibujados están bien donde son un
// CRONÓMETRO —el temporizador, el reproductor— y mal donde son un DATO.

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "../activities/ListStyle.h"
#include "../fontIds.h"

namespace heroui {

// La unidad va pegada al número y en UI_14: a la misma altura que el dígito se
// come el renglón, y es lo que menos hay que leer de los dos.
constexpr int UNIT_GAP = 4;

// Alto del bloque del número, para que el llamador reserve sin dibujarlo.
inline int heroHeight(GfxRenderer& renderer) {
  return renderer.getLineHeight(DISPLAY_32_FONT_ID) + renderer.getLineHeight(UI_10_FONT_ID);
}

// Dibuja `value` grande con `unit` chica al lado y `caption` debajo. Devuelve
// el alto usado. `value` sólo puede llevar dígitos y los seis símbolos de la
// cara de display; cualquier otra cosa no tiene glifo y no se ve.
inline int drawHero(GfxRenderer& renderer, const int x, const int y, const int maxW, const char* value,
                    const char* unit, const char* caption) {
  const int numH = renderer.getLineHeight(DISPLAY_32_FONT_ID);
  const int ui14H = renderer.getLineHeight(UI_14_FONT_ID);
  const int numW = renderer.getTextWidth(DISPLAY_32_FONT_ID, value, EpdFontFamily::BOLD);
  renderer.drawText(DISPLAY_32_FONT_ID, x, y, value, true, EpdFontFamily::BOLD);
  if (unit != nullptr && unit[0] != '\0') {
    // Alineada con la base del número, no con su tope: así se lee como una
    // unidad y no como un exponente.
    renderer.drawText(UI_14_FONT_ID, x + numW + UNIT_GAP, y + numH - ui14H - 2, unit, true, EpdFontFamily::BOLD);
  }
  int used = numH;
  if (caption != nullptr && caption[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, x, y + numH - 4, renderer.truncatedText(UI_10_FONT_ID, caption, maxW).c_str(),
                      true, EpdFontFamily::BOLD);
    used += renderer.getLineHeight(UI_10_FONT_ID) - 4;
  }
  return used;
}

struct Stat {
  const char* label;
  std::string value;
};

// Alto de la fila de medidas.
inline int statsHeight(GfxRenderer& renderer) {
  return renderer.getLineHeight(SMALL_FONT_ID) + renderer.getLineHeight(UI_12_FONT_ID) + 12;
}

// Una fila de medidas en columnas iguales: etiqueta chica arriba, valor debajo,
// con una regla de 1 px entre columnas. La jerarquía la hace la tipografía, no
// las cajas (principio 2 del sistema visual), así que no hay marcos.
inline void drawStats(GfxRenderer& renderer, const int x, const int y, const int w, const Stat* stats, const int n) {
  if (n <= 0) return;
  const int colW = w / n;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  for (int i = 0; i < n; ++i) {
    const int cx = x + i * colW;
    // La regla va en el hueco ENTRE columnas, no sobre el borde de la de al
    // lado: pegada al texto se lee como un marco y el sistema visual usa
    // reglas, no cajas.
    if (i > 0) renderer.fillRect(cx - 8, y + 2, 1, labelH + valueH + 4, true);
    const int inner = colW - 12;
    renderer.drawText(SMALL_FONT_ID, cx, y, renderer.truncatedText(SMALL_FONT_ID, stats[i].label, inner).c_str());
    renderer.drawText(UI_12_FONT_ID, cx, y + labelH + 2,
                      renderer.truncatedText(UI_12_FONT_ID, stats[i].value.c_str(), inner).c_str(), true,
                      EpdFontFamily::BOLD);
  }
}

}  // namespace heroui
