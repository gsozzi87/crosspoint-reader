#pragma once

#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

class GfxRenderer;

// Diario: el tema de la ws397, elegido por el usuario sobre tres propuestas
// maquetadas a tamaño real (docs/ws397/maquetas/).
//
// La idea es un diario impreso: serif para lo que se lee, antetítulos en
// versalita espaciada, reglas de 1 px en vez de marcos y cero pastillas negras.
// Conserva las métricas probadas de Lyra como base geométrica, pero usa los
// componentes rectos de BaseTheme: Diario no hereda pastillas ni tarjetas.
//
// El cambio grande no está acá sino en `ListStyle.h`: hasta 1.5.76 las caras de
// NUESTRAS listas estaban fijas, así que cambiar de tema cambiaba el lector y
// dejaba el hub, la agenda, las notas y las noticias exactamente igual — o sea
// que el tema no mandaba justo donde el usuario pasa el tiempo.
namespace DiarioMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // Lo que se lee, en serif; las etiquetas y los datos, en la sans chica.
  v.listTitleFont = NOTOSERIF_14_FONT_ID;
  v.listDetailFont = SMALL_FONT_ID;
  v.sectionTitleFont = NOTOSERIF_16_FONT_ID;
  v.sectionDatumFont = SMALL_FONT_ID;
  // La cabecera del diario: regla gruesa al pie del título, como el filete que
  // separa el cabezal de la primera columna.
  v.headerUnderlineSize = 3;
  v.headerTitleAlign = 0;  // a la izquierda, siempre
  v.listTitleBold = false;
  // Esquinas vivas: el lenguaje es impreso. Lyra trae 6 y acá no corresponde.
  v.listRowRadius = 0;
  v.listRowGap = 0;
  v.listInset = 0;
  v.listSidePadding = 24;
  v.listSelectionStyle = 2;
  v.buttonHintsBoxRadius = -1;
  v.popupCornerRadius = 0;
  v.controlRadius = 0;
  v.sheetRadius = 0;
  v.capsuleRadius = 0;
  return v;
}();
}  // namespace DiarioMetrics

class DiarioTheme : public BaseTheme {
 public:
  bool showsFileIcons() const override { return true; }
};
