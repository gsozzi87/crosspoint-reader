#pragma once

#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

// Bento: el otro tema maquetado del panel de propuestas
// (docs/ws397/maquetas/tema2-bento.png).
//
// Donde Diario es un diario impreso —serif, versalitas, filetes—, Bento es una
// caja de compartimentos: todo en la sans, cada cosa en su tarjeta de esquinas
// redondeadas, antetítulos cortos en negrita y nada de reglas. Es el mismo
// contenido leído de otra manera; la geometría no cambia, porque el margen de
// 24 px y la grilla de 8 son del sistema visual y no del tema.
namespace BentoMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // Todo en la sans, y el título de la fila en negrita: es lo que separa un
  // compartimento del siguiente cuando no hay reglas que los separen.
  v.listTitleFont = UI_12_FONT_ID;
  v.listDetailFont = SMALL_FONT_ID;
  v.sectionTitleFont = UI_14_FONT_ID;
  v.sectionDatumFont = SMALL_FONT_ID;
  v.listTitleBold = true;
  // Tarjetas: esquinas redondeadas y aire entre filas, sin filete al pie del
  // cabezal (la caja ya separa).
  v.listRowRadius = 14;
  v.listRowGap = 8;
  v.headerUnderlineSize = 0;
  v.buttonHintsBoxRadius = 12;
  return v;
}();
}  // namespace BentoMetrics

class BentoTheme : public LyraTheme {};
