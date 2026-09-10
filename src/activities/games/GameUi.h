#pragma once

#include <GfxRenderer.h>

#include <cstdint>

// El sistema visual de los juegos (1.5.48, docs/ws397/DISENO.md).
//
// Hasta 1.5.47 cada juego se inventaba su bloque de abajo: el marcador iba en
// SMALL (la fuente de los pies de página) a 22 px de la línea de antes, la ayuda
// salía cortada con puntos suspensivos porque era UNA línea centrada, y los
// tableros tramaban las casillas oscuras al 50 %, que en tinta electrónica
// vibra y se come la ficha. Acá está lo que usan TODOS nuestros juegos:
//
//   - un solo margen lateral de 24 px y todo en la grilla de 8;
//   - el ESTADO ("Tu turno", "Pensando...") en UI_14, con el detalle de la
//     selección alineado a la derecha sobre la misma línea de base;
//   - los MARCADORES en una franja de columnas, el número en UI_14 y la
//     etiqueta en SMALL debajo: entre jugada y jugada es lo único que se mira,
//     así que no puede estar en la fuente más chica de la pantalla;
//   - la AYUDA repartida en hasta TRES renglones con `wrappedText`, y con el
//     alto SIEMPRE reservado: así ninguna ayuda sale truncada en ningún idioma
//     (el alemán y el ruso son un tercio más largos que el español) y el
//     tablero no cambia de tamaño según lo que diga el renglón de abajo, que en
//     e-ink significaría repintar el tablero entero por un cambio de texto;
//   - las superficies se separan con REGLAS de 1 px, nunca con marcos ni áreas
//     rellenas.
//
// Todo lo vertical sale de `getLineHeight`/`getFontAscenderSize` de las fuentes
// reales: nada de literales que se pisan cuando cambia una cara.
//
// La implementación va en GameUi.cpp y no acá: en la cabecera, `inline` las
// duplicaba en las siete Activities que las usan y el binario está al 88 % de
// la flash.
namespace gameui {

constexpr int SIDE = 24;        // ÚNICO margen lateral de la pantalla
constexpr int GAP = 8;          // paso de la grilla
constexpr int HELP_LINES = 3;   // renglones RESERVADOS para la ayuda (se usen o no)
constexpr int CURSOR_W = 4;     // marco del cursor
constexpr int LAST_MOVE_W = 2;  // marco de la última jugada de la máquina

// Una regla de 1 px separa dos superficies mejor que un marco y no deja
// fantasma en el refresco parcial siguiente.
void rule(const GfxRenderer& renderer, int x, int y, int w);

int contentWidth(const GfxRenderer& renderer);

// ------------------------------------------------------------------ ayuda ---

int helpHeight(const GfxRenderer& renderer);

// La ayuda de abajo: qué hace cada botón AHORA. Va en UI_10 (es texto, no un
// pie de página) y centrada, repartida en los renglones que haga falta.
void help(const GfxRenderer& renderer, int y, const char* text);

// ----------------------------------------------------------------- estado ---

int statusHeight(const GfxRenderer& renderer);

// Estado de la partida: el título en UI_14 a la izquierda y el detalle de la
// selección ("Elige una ficha · 3 de 5") en UI_10 a la derecha, los dos sobre
// la MISMA línea de base.
//
// El ancho se reparte MIDIENDO, no por mitades fijas: el detalle termina en el
// contador, que es lo único que dice si la palanca movió el cursor, así que se
// queda con todo lo que el título no usa y sólo se recorta cuando ni siquiera
// así entra (hasta 1.5.48 el tope era w/2 y en francés, portugués y ruso el
// contador desaparecía siempre).
void status(const GfxRenderer& renderer, int x, int y, int w, const char* title, const char* detail);

// ------------------------------------------------------------- marcadores ---

// Una columna de la franja: el número arriba y qué es debajo. `weight` reparte
// el ancho: una notación de ajedrez ("42. e7e8=D") no entra en el mismo ancho
// que un "12", así que esa columna pide 2 y las de material 1.
struct Stat {
  const char* value = "";
  const char* label = "";
  uint8_t weight = 1;
};

// `labelLines` son los renglones RESERVADOS para la etiqueta. Con cuatro
// columnas (108 px cada una) etiquetas como "ваши фигуры" o "as tuas peças" no
// entran en un renglón, así que ese caso pide 2 y la etiqueta se parte en vez
// de salir con puntos suspensivos.
int statsHeight(const GfxRenderer& renderer, int labelLines = 1);

// La franja de marcadores: regla de 1 px arriba, hasta cuatro columnas
// separadas por reglas verticales de 1 px, el valor en UI_14 y la etiqueta en
// SMALL. Sin marcos y sin nada relleno: el peso lo hace la tipografía.
void stats(const GfxRenderer& renderer, int x, int y, int w, const Stat* items, int count,
           int labelLines = 1);

// ---------------------------------------------------------------- tablero ---

// Casilla oscura al 25 %. Al 50 % (DarkGray, un píxel de cada dos) el tablero
// vibra, se pelea con la ficha y deja fantasma; al 25 % se lee como un gris
// tranquilo y la ficha se despega sola.
void shadeCell(const GfxRenderer& renderer, int x, int y, int cell);

// El cursor: marco de 4 px pegado al borde de la casilla. NUNCA relleno
// macizo: taparía la ficha y dejaría un manchón en el parcial siguiente.
void cursorFrame(const GfxRenderer& renderer, int x, int y, int cell);

// La última jugada de la máquina, en la casilla de origen y en la de destino.
// Hasta 1.5.47 eran dos cuadraditos de 6 o 7 px en las esquinas: invisibles, así
// que el usuario no sabía qué había movido el aparato. Un marco de 2 px por
// dentro de la casilla se ve de lejos y no se confunde con el cursor (4 px y
// pegado al borde) ni con las escuadras de los candidatos.
//
// Va DESPUÉS de las fichas: el halo blanco de la ficha llega casi al borde de
// la casilla y, dibujado antes, le borraría el medio de cada lado al marco.
void lastMoveFrame(const GfxRenderer& renderer, int x, int y, int cell);

}  // namespace gameui
