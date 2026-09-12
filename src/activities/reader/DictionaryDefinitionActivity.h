#pragma once

#include <EpdFontFamily.h>
#include <Epub/Page.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// El visor de texto largo del aparato. Nació para una definición de
// diccionario, pero hoy es la pantalla donde se lee TODO lo que no es un libro:
// la respuesta de "Preguntarle al libro", la de Hablar, una nota, un capítulo
// de la Biblia.
//
// Rediseño 1.5.48 (docs/ws397/DISENO.md). Los tres defectos que marcó el panel
// de jueces estaban acá:
//
//   - el paso de renglón era el de la fuente de lectura (~34 px) y el bloque
//     quedaba apretado: ahora es de 40 px fijos, que es lo que hace que una
//     respuesta larga se lea de un tirón;
//   - el título iba en UI_12 y no se distinguía del cuerpo: va en UI_14, con
//     una regla de 1 px debajo que separa el encabezado del texto sin marcos;
//   - el paginador era un folio "2 / 5" en una esquina que no entiende nadie:
//     ahora dice "Página 2 de 5" con una barra que se llena (listui::pager).
//
// Las definiciones HTML siguen componiéndose con el motor del lector (Pages);
// todo lo demás se corta en renglones una sola vez al entrar y cada página
// dibuja tramos de la cadena original, sin copias por renglón.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, bool verseNumbers = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        verseNumbers(verseNumbers) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Atrás MANTENIDO como botón de voz: el que abre el visor (la Biblia) le
  // pasa qué hacer y cómo se llama en la barra de abajo. El visor avisa y se
  // cierra; el dueño decide qué abrir con lo que estaba leyendo.
  void setVoiceHold(const char* label, std::function<void()> fn) {
    voiceHoldLabel = label;
    onVoiceHold = std::move(fn);
  }

 private:
  const char* voiceHoldLabel = nullptr;
  std::function<void()> onVoiceHold;
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
    // Bytes del número de versículo con los que arranca el renglón (0 = no hay).
    // Van en SMALL negrita, que tiene las mismas métricas que la regular y por
    // eso entra en el mismo renglón sin mover nada.
    uint16_t verseLen;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  // Ancho útil de la columna de texto (el margen único de 24 px ya descontado).
  int columnWidth() const;
  // Alto del encabezado: margen de arriba + el título (uno o dos renglones) +
  // la regla + el aire hasta el primer renglón del cuerpo.
  int headerHeight() const;
  // Primer píxel del renglón del paginador. El pie entero se deriva de acá
  // (footerHeight() y el dibujo salen de la misma cuenta, así que no pueden
  // separarse) y lleva verticalSpacing ADEMÁS del alto de los hints, igual que
  // listui::contentBottom: con el hueco justo el paginador queda pegado a la
  // barra de botones y parece parte de la botonera.
  int pagerTop() const;
  // Alto reservado abajo: aire + paginador + barra de botones.
  int footerHeight() const;
  void layoutTitle();
  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style style) const;
  void drawBody(int fontId, int x, int startY) const;

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  // Modo Biblia: el número que abre cada versículo se dibuja en SMALL negrita.
  const bool verseNumbers;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  // El título ya cortado en uno o dos renglones de UI_14 (con puntos suspensivos
  // si no entra). Se calcula al entrar porque el alto del encabezado —y con él
  // cuántos renglones entran— depende de cuántos ocupa.
  std::vector<std::string> titleLines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  // Paso de renglón del cuerpo. 40 px salvo que la cara de lectura elegida sea
  // más alta, en cuyo caso manda la fuente (si no, los glifos se pisan).
  int lineStep = 40;
  // Parciales desde el último refresco limpio (regla del panel: 10-15).
  int partialCount = 0;
  ButtonNavigator buttonNavigator;
};
