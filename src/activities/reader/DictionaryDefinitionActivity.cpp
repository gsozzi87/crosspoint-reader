#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "activities/ListStyle.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/GrayText.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Margen lateral ÚNICO de la pantalla (el mismo que las listas).
constexpr int SIDE_PADDING = listui::SIDE;

// Encabezado: aire de arriba, aire entre la última línea del título y la regla,
// y aire entre la regla y el primer renglón del cuerpo. Todo en la grilla de 8.
constexpr int TITLE_TOP = 16;
constexpr int TITLE_RULE_GAP = 6;
constexpr int BODY_TOP_GAP = 16;
constexpr int MAX_TITLE_LINES = 2;

// Paso de renglón del cuerpo. Los jueces midieron los tres visores de las
// maquetas y el cómodo fue el de 40: con el ~34 que daba la cara de lectura el
// bloque se lee como un párrafo apretado y se pierde el renglón.
constexpr int BODY_LINE_STEP = 40;

// Aire entre el último renglón y el paginador.
constexpr int PAGER_GAP = 8;

// Máximo de bytes del número de versículo ("119" y de sobra).
constexpr uint16_t MAX_VERSE_DIGITS = 3;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  // El título va primero: de cuántos renglones ocupe depende dónde empieza el
  // cuerpo, y de eso dependen el corte de renglones y la cuenta de páginas.
  layoutTitle();
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
  // Se entra desde otra pantalla completamente distinta: el primer dibujo va
  // con refresco limpio para no arrastrar lo que había antes.
  partialCount = GrayText::PARTIALS_BEFORE_CLEAN - 1;
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

int DictionaryDefinitionActivity::columnWidth() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  return renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING;
}

// El título de esta pantalla es una pregunta dictada o una referencia bíblica:
// puede ser largo. Entra en dos renglones de UI_14 y lo que sobre se corta con
// puntos suspensivos, que es mejor que empujar el cuerpo hacia abajo sin techo.
void DictionaryDefinitionActivity::layoutTitle() {
  titleLines.clear();
  if (headword.empty()) return;
  const int width = columnWidth();
  if (width <= 0) return;
  titleLines = renderer.wrappedText(UI_14_FONT_ID, headword.c_str(), width, MAX_TITLE_LINES);
}

int DictionaryDefinitionActivity::headerHeight() const {
  if (titleLines.empty()) return TITLE_TOP;
  return TITLE_TOP + static_cast<int>(titleLines.size()) * renderer.getLineHeight(UI_14_FONT_ID) + TITLE_RULE_GAP + 1 +
         BODY_TOP_GAP;
}

int DictionaryDefinitionActivity::pagerTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  // Dado vuelta la barra de botones queda arriba, así que abajo solo va el
  // paginador; el hueco de los hints ya lo descuenta bodyArea() por arriba.
  // El verticalSpacing va igual en las dos orientaciones: es el margen de abajo
  // de la pantalla, no el aire de los hints.
  const int hints = isInverted ? 0 : metrics.buttonHintsHeight;
  return renderer.getScreenHeight() - hints - metrics.verticalSpacing - renderer.getLineHeight(UI_10_FONT_ID);
}

int DictionaryDefinitionActivity::footerHeight() const {
  return renderer.getScreenHeight() - pagerTop() + PAGER_GAP;
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topArea = (isInverted ? metrics.buttonHintsHeight : 0) + headerHeight();
  return {columnWidth(), renderer.getScreenHeight() - topArea - footerHeight()};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len,
                                              const EpdFontFamily::Style style) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, style);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
//
// En modo Biblia, el número que abre cada versículo se mide (y después se
// dibuja) con SMALL en negrita: es el dato que deja seguir una cita sin que un
// número del tamaño del texto corte la lectura.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  // 40 px salvo que la cara elegida sea más alta: ahí manda la fuente, o los
  // renglones se pisan.
  lineStep = std::max(BODY_LINE_STEP, renderer.getLineHeight(fontId));
  linesPerPage = std::max(1, body.height / lineStep);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;
  uint16_t verseLen = 0;      // número de versículo del renglón que se está armando
  bool paragraphStart = true;  // el próximo token abre un versículo

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart), verseLen});
    verseLen = 0;
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      paragraphStart = true;
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;

    // ¿Es el número que abre un versículo? Solo si abre el párrafo, abre el
    // renglón, son todos dígitos y hay texto detrás (un "3" suelto al final no
    // es una cita, es parte del texto).
    bool isVerse = false;
    if (verseNumbers && paragraphStart && lineEnd == lineStart && tokenLen > 0 && tokenLen <= MAX_VERSE_DIGITS) {
      isVerse = true;
      for (uint32_t d = 0; d < tokenLen; d++) {
        if (text[tokenStart + d] < '0' || text[tokenStart + d] > '9') {
          isVerse = false;
          break;
        }
      }
      if (isVerse && (i >= n || text[i] == '\n' || text[i] == '\0')) isVerse = false;
    }
    paragraphStart = false;
    const int tokenWidth = isVerse ? measureSpan(SMALL_FONT_ID, text + tokenStart, tokenLen, EpdFontFamily::BOLD)
                                   : measureSpan(fontId, text + tokenStart, tokenLen, EpdFontFamily::REGULAR);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
      if (isVerse) verseLen = static_cast<uint16_t>(tokenLen);
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward. Un número de
    // versículo nunca llega acá (mide tres dígitos), así que el corte puede
    // medir con la cara de lectura sin más.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f, EpdFontFamily::REGULAR) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart, EpdFontFamily::REGULAR);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  // El número de versículo se apoya en la MISMA línea de base que el texto: por
  // eso baja la diferencia de ascendentes en vez de dibujarse en el tope.
  const int baselineDrop = renderer.getFontAscenderSize(fontId) - renderer.getFontAscenderSize(SMALL_FONT_ID);
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    const int y = startY + (i - firstLine) * lineStep;
    size_t offset = 0;
    int textX = x;
    if (lines[i].verseLen > 0 && lines[i].verseLen < len) {
      char number[MAX_VERSE_DIGITS + 1];
      memcpy(number, buf, lines[i].verseLen);
      number[lines[i].verseLen] = '\0';
      renderer.drawText(SMALL_FONT_ID, textX, y + baselineDrop, number, true, EpdFontFamily::BOLD);
      textX += renderer.getTextAdvanceX(SMALL_FONT_ID, number, EpdFontFamily::BOLD) + spaceWidth;
      offset = lines[i].verseLen;
      while (offset < len && (buf[offset] == ' ' || buf[offset] == '\t')) offset++;
    }
    renderer.drawText(fontId, textX, y, buf + offset);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int x = contentX + SIDE_PADDING;
  const int w = columnWidth();

  // Encabezado: el título en UI_14 (la pregunta dictada, la referencia del
  // capítulo, el nombre de la nota) y una regla de 1 px que lo separa del
  // cuerpo. Nada de marcos ni de barras rellenas.
  int titleY = contentY + TITLE_TOP;
  for (const std::string& line : titleLines) {
    renderer.drawText(UI_14_FONT_ID, x, titleY, line.c_str());
    titleY += renderer.getLineHeight(UI_14_FONT_ID);
  }
  if (!titleLines.empty()) listui::rule(renderer, x, titleY + TITLE_RULE_GAP, w);

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = contentY + headerHeight();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, x, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, x, bodyStartY);

  // Paginador: "Página 2 de 5" y la barra que se llena. La palabra "Página" es
  // lo que explica la barra; el folio "2 / 5" en una esquina no lo entendía nadie.
  listui::pager(renderer, x, pagerTop(), w, currentPage + 1, totalPages);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", (currentPage > 0 ? tr(STR_DIR_UP) : ""),
                                            (currentPage + 1 < totalPages ? tr(STR_DIR_DOWN) : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Esta pantalla es para leer (capítulos de la Biblia, respuestas, noticias),
  // así que el texto sale por el pipeline de grises igual que en el lector: la
  // base en blanco y negro y encima las dos pasadas de suavizado. Solo se
  // vuelve a dibujar el cuerpo; el encabezado y los botones quedan de la base.
  GrayText::displayPage(renderer, partialCount, [&] { drawBody(fontId, x, bodyStartY); });
}
