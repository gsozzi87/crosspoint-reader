#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/Selection.h"
#include "components/themes/BaseTheme.h"
#include "components/UITheme.h"
#include "fontIds.h"

// El sistema visual de las listas (1.5.48, docs/ws397/DISENO.md).
//
// Hasta 1.5.47 cada pantalla tenía su propio margen (10, 12, 18, 20, 22, 24) y
// su propio alto de fila (34, 40, 44, 56, 60, 74), y el texto entraba entero en
// un renglón corrido: "Sacar la basura hoy 20:00 De lunes a viernes". Acá está
// la fila que usan TODAS nuestras listas, con las reglas del rediseño:
//
//   - un solo margen lateral de 24 px y todo en la grilla de 8;
//   - la fila es de DOS renglones: el título en UI_12 y el detalle (cuándo,
//     cada cuánto, cuánto dura) en UI_10 debajo, nunca todo seguido;
//   - el metadato va alineado a la DERECHA en su columna, no pegado al título;
//   - la casilla de 18x18 px a la izquierda es la que explica sin manual que OK
//     tilda ese ítem;
//   - jerarquía por tipografía y reglas de 1 px, nunca marcos ni áreas llenas;
//   - NUNCA hay letras sobre trama: el texto arranca a PAD (24 px) del borde de
//     la fila, o sea por dentro de las franjas de 16 px que dibuja el resalte
//     (`drawSelectionRow` las pone en [x+3, x+19] y [x+w-19, x+w-3]).
//
// Todo lo vertical se deriva de `getLineHeight`/`getFontAscenderSize` de las
// fuentes reales: nada de literales de línea base que se pisan cuando cambia
// una cara.
namespace listui {

constexpr int SIDE = 24;      // ÚNICO margen lateral de la pantalla
constexpr int PAD = 24;       // borde de la fila -> texto (deja libre la franja del resalte)
constexpr int ROW1_H = 48;    // fila de un renglón
constexpr int ROW2_H = 72;    // fila de dos renglones
// Encabezado de sección: UI_14 mide 34 de línea (28 de ascendente y 6 de
// descendente), así que la regla del pie va a 40 y no a 32: con 32 la línea
// cortaba la panza de las "p" y las "g".
constexpr int SECTION_H = 40;
constexpr int HINT_H = 24;    // renglón de ayuda arriba de la botonera
constexpr int CHECK = 18;     // casilla "OK lo tilda"
constexpr int CHECK_FRAME = 2;
constexpr int CHECK_GAP = 14;  // casilla -> título
constexpr int META_GAP = 16;   // título -> metadato de la derecha
constexpr int GAP = 8;         // paso de la grilla

// Primer y último píxel útiles de la columna de contenido. El margen de abajo
// lleva verticalSpacing ADEMÁS del alto de los hints: con el hueco justo, la
// última fila queda pegada a la barra de botones y parece tapada.
inline int contentTop() {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + GAP;
}

inline int contentBottom(const GfxRenderer& renderer) {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing;
}

inline int contentWidth(const GfxRenderer& renderer) { return renderer.getScreenWidth() - 2 * SIDE; }

// Una regla de 1 px separa dos superficies mejor que un marco y cuesta la
// centésima parte de tinta (no deja fantasma en el parcial siguiente).
inline void rule(const GfxRenderer& renderer, const int x, const int y, const int w) {
  renderer.fillRect(x, y, w, 1, true);
}

// El helper de recorte marca lo que NO entró con la elipsis U+2026 (3 bytes) y,
// cuando el texto entra completo, lo devuelve tal cual. Sin mirar la elipsis no
// hay forma de distinguir los dos casos: un texto corto terminaba mostrando sus
// propios últimos 3 bytes como segundo renglón, partidos al medio si acababa en
// un carácter de más de un byte.
inline std::string tailAfterEllipsis(const std::string& shown, const std::string& full) {
  static const char* const ELLIPSIS = "\xe2\x80\xa6";
  if (shown.size() <= 3 || shown.compare(shown.size() - 3, 3, ELLIPSIS) != 0) return {};
  const size_t cut = shown.size() - 3;  // truncatedText corta en frontera de carácter
  if (cut >= full.size()) return {};
  return full.substr(cut);
}

// Encabezado de sección: UI_14 a la izquierda, dato opcional alineado a la
// derecha sobre la MISMA línea de base, y una regla de 1 px al pie. Devuelve la
// y donde sigue el contenido.
//
// UI_14 se pide en REGULAR a propósito: la familia se armó con una sola cara
// (la negrita), así que REGULAR y BOLD dan el mismo glifo y REGULAR no paga la
// búsqueda de una cara que no existe.
inline int sectionHeader(const GfxRenderer& renderer, const int x, const int y, const int w, const char* title,
                         const char* datum = nullptr) {
  // El dato viene de afuera (nombre de archivo del teléfono, título de un feed):
  // medirlo sin recortarlo se come el rótulo y se sale de la pantalla por la
  // izquierda. Media caja para el dato como mucho, y el origen nunca a la
  // izquierda del margen.
  const std::string datumText = datum && *datum ? renderer.truncatedText(SMALL_FONT_ID, datum, w / 2) : std::string();
  const int datumW = datumText.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, datumText.c_str());
  const int titleW = w - (datumW > 0 ? datumW + META_GAP : 0);
  const int titleY = y + 3;
  renderer.drawText(UI_14_FONT_ID, x, titleY, renderer.truncatedText(UI_14_FONT_ID, title, titleW).c_str());
  if (datumW > 0) {
    const int baseY = titleY + renderer.getFontAscenderSize(UI_14_FONT_ID) - renderer.getFontAscenderSize(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, std::max(x, x + w - datumW), baseY, datumText.c_str());
  }
  rule(renderer, x, y + SECTION_H - 1, w);
  return y + SECTION_H;
}

// La casilla vacía de "OK lo tilda". Cuando el ítem se tilda antes de irse de la
// lista, la tilde va a mano con dos líneas: no hay glifo de tilde en las fuentes
// de UI y un ícono de 18 px para esto no vale el flash.
inline void checkBox(const GfxRenderer& renderer, const int x, const int y, const bool checked = false) {
  renderer.drawRect(x, y, CHECK, CHECK, CHECK_FRAME, true);
  if (!checked) return;
  renderer.drawLine(x + 4, y + CHECK / 2, x + CHECK / 2 - 1, y + CHECK - 5, 2, true);
  renderer.drawLine(x + CHECK / 2 - 1, y + CHECK - 5, x + CHECK - 4, y + 4, 2, true);
}

// Lo que se puede poner en una fila. Todo lo opcional se dibuja solo si está.
struct RowSpec {
  const char* title = "";
  const freeink::Icon* icon = nullptr;  // ícono de 24 px a la izquierda del título
  const char* detail = nullptr;  // segundo renglón (fecha, repetición, estado)
  const char* meta = nullptr;    // metadato alineado a la derecha (hora, duración)
  bool selected = false;
  bool bold = false;     // filas de acción ("+ Nueva nota")
  bool check = false;    // dibuja la casilla de "OK lo tilda"
  bool checked = false;
  bool metaBox = false;  // el metadato adentro de un marco fino (la cuenta de una sección)
  bool rule = true;      // regla de 1 px al pie (la fila elegida ya trae marco)
};

// La fila entera. `h` es ROW1_H o ROW2_H (o lo que la pantalla pueda darle: los
// renglones se centran solos en el alto que venga).
inline void row(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const RowSpec& spec) {
  if (spec.selected) {
    // radio 0: el lenguaje es impreso, esquinas vivas. El resalte deja el
    // centro en blanco, así que el texto de abajo se lee igual que el resto.
    drawSelectionRow(renderer, x, y, w, h, 0);
  } else if (spec.rule) {
    listui::rule(renderer, x, y + h - 1, w);
  }

  const EpdFontFamily::Style titleStyle = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const char* title = spec.title ? spec.title : "";
  const int lineTitle = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineDetail = renderer.getLineHeight(UI_10_FONT_ID);
  const bool twoLines = spec.detail && *spec.detail;
  const int block = twoLines ? lineTitle + lineDetail : lineTitle;
  // Si la pantalla le dio menos alto del que piden los dos renglones, el bloque
  // arranca arriba de todo en vez de salirse por arriba de la fila.
  const int titleY = y + std::max(0, (h - block) / 2);
  const int ascTitle = renderer.getFontAscenderSize(UI_12_FONT_ID);
  const int ascMeta = renderer.getFontAscenderSize(UI_10_FONT_ID);

  int textX = x + PAD;
  if (spec.icon) {
    // Apoyado en la línea de base del título, como una letra más alta.
    BaseTheme::drawIconBitmap(renderer, *spec.icon, textX, titleY + ascTitle - 2 - spec.icon->h, SELECTION_INK);
    textX += spec.icon->w + CHECK_GAP;
  }
  if (spec.check) {
    // La casilla se apoya en la línea de base del título, como una letra más.
    checkBox(renderer, textX, titleY + ascTitle - 2 - CHECK, spec.checked);
    textX += CHECK + CHECK_GAP;
  }

  const int right = x + w - PAD;
  int metaW = 0;
  if (spec.meta && *spec.meta) {
    // El metadato se queda con tres quintos de la fila como mucho: lo que se
    // corta es el valor largo, nunca el título, que es lo que identifica la fila.
    const std::string text = renderer.truncatedText(UI_10_FONT_ID, spec.meta, (w - 2 * PAD) * 3 / 5);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
    const int metaY = titleY + ascTitle - ascMeta;
    if (spec.metaBox) {
      const int boxW = std::max(28, textW + 16);
      const int boxH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
      renderer.drawRect(right - boxW, metaY - 2, boxW, boxH, 1, true);
      renderer.drawText(UI_10_FONT_ID, right - boxW + (boxW - textW) / 2, metaY, text.c_str(), SELECTION_INK);
      metaW = boxW;
    } else {
      renderer.drawText(UI_10_FONT_ID, right - textW, metaY, text.c_str(), SELECTION_INK);
      metaW = textW;
    }
    metaW += META_GAP;
  }

  const int titleW = right - metaW - textX;
  if (titleW > 0) {
    renderer.drawText(UI_12_FONT_ID, textX, titleY,
                      renderer.truncatedText(UI_12_FONT_ID, title, titleW, titleStyle).c_str(), SELECTION_INK,
                      titleStyle);
  }
  // El segundo renglón usa todo el ancho salvo cuando el metadato va en un
  // marco: ahí la caja baja hasta la línea del detalle y se le deja el lugar.
  const int detailW = right - textX - (spec.metaBox ? metaW : 0);
  if (twoLines && detailW > 0) {
    renderer.drawText(UI_10_FONT_ID, textX, titleY + lineTitle,
                      renderer.truncatedText(UI_10_FONT_ID, spec.detail, detailW).c_str(), SELECTION_INK);
  }
}

// El renglón de ayuda: qué hace OK acá y qué hay escondido en Atrás mantenido.
// Va en UI_10 (la ayuda es texto, no un pie de página) y centrado.
inline void hint(const GfxRenderer& renderer, const int y, const char* text) {
  if (!text || !*text) return;
  const int w = contentWidth(renderer);
  renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, text, w).c_str());
}

// El paginador. "2 / 5" en una esquina no lo entiende nadie: va la frase entera
// ("Página 2 de 5") y al lado una barra que se llena, que es lo que se lee de
// un vistazo. Carril de 1 px y tramo hecho de 3 px, para no dejar fantasma.
//
// `fmt` deja cambiar la frase donde la unidad no es una página (el artículo de
// Noticias va por partes), sin que cada pantalla se arme la barra a mano.
inline void pager(const GfxRenderer& renderer, const int x, const int y, const int w, const int rawPage,
                  const int pages, const char* fmt = nullptr) {
  if (pages <= 1) return;
  // La cuenta de páginas cambia cuando cambia el alto de las filas: el número
  // se acota acá y no en cada pantalla.
  const int page = std::min(std::max(rawPage, 1), pages);
  char text[48];
  snprintf(text, sizeof(text), fmt && *fmt ? fmt : tr(STR_PAGE_FORMAT), page, pages);
  const int textW = renderer.getTextWidth(UI_10_FONT_ID, text);
  renderer.drawText(UI_10_FONT_ID, x, y, text);
  const int barX = x + textW + META_GAP;
  const int barW = w - textW - META_GAP;
  if (barW < 40) return;
  const int barY = y + renderer.getFontAscenderSize(UI_10_FONT_ID) / 2;
  renderer.fillRect(barX, barY, barW, 1, true);
  const int done = std::max(1, barW * page / pages);
  renderer.fillRect(barX, barY - 1, done, 3, true);
}

}  // namespace listui
