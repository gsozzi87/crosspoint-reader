#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
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

// Cuánto queda el cartel de "no se encontró" / "no hay diccionario".
constexpr unsigned long POPUP_MS = 1500;

// Una palabra es elegible si tiene un alfanumérico ASCII o un codepoint fuera
// de U+2000-U+206F (guiones, viñetas y demás puntuación que aparece suelta no
// son palabras). Misma regla que el cursor del lector, acotada por largo
// porque acá el texto es un bloque sin cortes.
bool isSelectableToken(const char* text, const size_t len) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i < len; i++) {
    if (p[i] < 0x80) {
      if (std::isalnum(p[i])) return true;
    } else if (p[i] == 0xE2 && i + 2 < len && (p[i + 1] == 0x80 || p[i + 1] == 0x81)) {
      i += 2;  // codepoint de General Punctuation: se saltea entero
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

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

int DictionaryDefinitionActivity::footerHeight() const { return renderer.getScreenHeight() - pagerTop() + PAGER_GAP; }

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
  uint16_t verseLen = 0;       // número de versículo del renglón que se está armando
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

int DictionaryDefinitionActivity::textLeft() const {
  const auto orientation = renderer.getOrientation();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gutter = orientation == GfxRenderer::Orientation::LandscapeClockwise ? metrics.sideButtonHintsWidth : 0;
  return gutter + SIDE_PADDING;
}

int DictionaryDefinitionActivity::bodyTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  return (isInverted ? metrics.buttonHintsHeight : 0) + headerHeight();
}

void DictionaryDefinitionActivity::addMenuItem(const char* label, std::function<void()> fn) {
  menuItems.push_back({label ? label : "", std::move(fn)});
}

// El menú de OK. "Buscar una palabra" va primero y siempre: es lo que el visor
// puede ofrecer por sí mismo, porque es el único que sabe dónde cayó cada
// palabra en el vidrio.
void DictionaryDefinitionActivity::openMenu() {
  menuLabels.clear();
  menuLabels.reserve(menuItems.size() + 1);
  menuLabels.push_back(tr(STR_DICT_LOOKUP_WORD));
  for (const MenuItem& item : menuItems) menuLabels.push_back(item.label);
  menu.show(StrId::STR_TEXT_MENU_TITLE, menuLabels, 0, [this](const int idx) {
    if (idx == 0) {
      // Buscar significa diccionario local y nunca se convierte en una consulta
      // a la IA sin avisar: si no hay diccionario se dice, igual que el lector.
      if (!dictlookup::available()) {
        popup = Popup::Message;
        popupMsg = StrId::STR_DICT_NO_DICT_SET;
        popupTime = millis();
        requestUpdate();
        return;
      }
      buildPageWords();
      if (pageWords.empty()) {
        popup = Popup::Message;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        popupTime = millis();
        requestUpdate();
        return;
      }
      mode = Mode::Words;
      wordIndex = 0;
      wordsNeedClean = true;
      requestUpdate();
      return;
    }
    const size_t at = static_cast<size_t>(idx) - 1;
    if (at < menuItems.size() && menuItems[at].fn) menuItems[at].fn();
    leaving = true;
    finish();
  });
  requestUpdate();
}

// Las palabras de la página que está EN EL VIDRIO, con la posición en la que se
// dibujaron. Sale de los mismos renglones y las mismas medidas que drawBody, no
// de una segunda maquetación: si fueran dos cuentas, el resalte caería en otro
// lado que la letra.
void DictionaryDefinitionActivity::buildPageWords() {
  pageWords.clear();
  wordIndex = 0;
  // Camino HTML (una definición del diccionario compuesta con el motor del
  // lector): ahí el texto vive en las Pages y no en `lines`. Buscar una palabra
  // adentro de una definición tampoco es lo que nadie pide.
  if (!pages.empty() || lines.empty()) return;

  const int fontId = SETTINGS.getReaderFontId();
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int x0 = textLeft();
  const int y0 = bodyTop();
  const char* text = definition.c_str();
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));

  for (int i = firstLine; i < lastLine; i++) {
    // Lo que se dibuja de un renglón está topeado en MAX_LINE_BYTES (drawBody
    // copia a un buffer de ese tamaño), así que más allá de ahí no hay nada en
    // pantalla que elegir.
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    if (len == 0) continue;
    const uint32_t base = lines[i].start;
    const int y = y0 + (i - firstLine) * lineStep;

    size_t offset = 0;
    int penX = x0;
    if (lines[i].verseLen > 0 && lines[i].verseLen < len) {
      char number[MAX_VERSE_DIGITS + 1];
      memcpy(number, text + base, lines[i].verseLen);
      number[lines[i].verseLen] = '\0';
      penX += renderer.getTextAdvanceX(SMALL_FONT_ID, number, EpdFontFamily::BOLD) + spaceWidth;
      offset = lines[i].verseLen;
      while (offset < len && (text[base + offset] == ' ' || text[base + offset] == '\t')) offset++;
    }

    size_t at = offset;
    while (at < len) {
      while (at < len && (text[base + at] == ' ' || text[base + at] == '\t')) at++;
      if (at >= len) break;
      const size_t tokenStart = at;
      while (at < len && text[base + at] != ' ' && text[base + at] != '\t') at++;
      const size_t tokenLen = at - tokenStart;
      if (!isSelectableToken(text + base + tokenStart, tokenLen)) continue;
      // El resto del renglón se dibuja de un tirón desde penX, así que cada
      // palabra cae en penX más el ancho de lo que va antes (espacios incluidos).
      const int before = measureSpan(fontId, text + base + offset, tokenStart - offset, EpdFontFamily::REGULAR);
      const int width = measureSpan(fontId, text + base + tokenStart, tokenLen, EpdFontFamily::REGULAR);
      pageWords.push_back({static_cast<uint32_t>(base + tokenStart), static_cast<uint16_t>(tokenLen),
                           static_cast<int16_t>(penX + before), static_cast<int16_t>(y), static_cast<int16_t>(width)});
    }
  }
}

size_t DictionaryDefinitionActivity::wordText(const WordBox& w, char* out, const size_t cap) const {
  const size_t len = std::min(static_cast<size_t>(w.len), cap - 1);
  memcpy(out, definition.c_str() + w.start, len);
  out[len] = '\0';
  return len;
}

void DictionaryDefinitionActivity::performLookup() {
  if (pageWords.empty() || wordIndex >= static_cast<int>(pageWords.size())) return;
  char word[96];
  if (wordText(pageWords[wordIndex], word, sizeof(word)) == 0) return;

  popup = Popup::Busy;
  popupMsg = dict.busyMessage();
  requestUpdateAndWait();  // que el cartel esté en el vidrio antes de bloquear en la SD

  dictlookup::Hit hit = dict.lookup(word, &indexBuildYield, nullptr);
  if (hit.found) {
    popup = Popup::None;
    startActivityForResult(makeUniqueNoThrow<DictionaryDefinitionActivity>(
                               renderer, mappedInput, std::move(hit.headword), std::move(hit.definition), hit.html),
                           [this](const ActivityResult&) {
                             partialCount = GrayText::PARTIALS_BEFORE_CLEAN - 1;
                             wordsNeedClean = true;  // la definición dejó su página entera en el vidrio
                             requestUpdate();
                           });
    return;
  }
  popup = Popup::Message;
  popupMsg = hit.message;
  popupTime = millis();
  requestUpdate();
}

// El resalte de la palabra elegida: caja negra y la palabra en blanco, igual
// que el cursor del lector. Acá no hay trama de por medio, así que la regla de
// "nunca letras sobre trama" se respeta sola.
void DictionaryDefinitionActivity::drawWordHighlight(const int fontId) const {
  if (pageWords.empty() || wordIndex >= static_cast<int>(pageWords.size())) return;
  const WordBox& w = pageWords[wordIndex];
  char word[96];
  const size_t len = std::min(static_cast<size_t>(w.len), sizeof(word) - 1);
  memcpy(word, definition.c_str() + w.start, len);
  word[len] = '\0';
  int hx = w.x - 2;
  int hy = w.y - 2;
  int hw = w.width + 4;
  int hh = renderer.getLineHeight(fontId) + 4;
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }
  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, w.x, w.y, word, false);
}

void DictionaryDefinitionActivity::loop() {
  if (leaving) return;

  // El cartel de "no se encontró" se va solo; mientras está, nada más responde.
  if (popup == Popup::Message) {
    if (millis() - popupTime >= POPUP_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (menu.isActive()) {
    menu.handleInput(mappedInput, [this] { requestUpdate(); });
    // Atrás en el menú lo cierra y no hace nada más: hay que volver a pintar la
    // página, que quedó tapada por el diálogo.
    if (!menu.isActive() && !leaving && popup == Popup::None && mode == Mode::Read) requestUpdate();
    return;
  }

  // Cursor de palabras (diccionario): la palanca recorre las palabras de la
  // página en orden de lectura, OK busca y Atrás vuelve al texto.
  if (mode == Mode::Words) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mode = Mode::Read;
      pageWords.clear();
      partialCount = GrayText::PARTIALS_BEFORE_CLEAN - 1;  // sacar el resalte pide un refresco limpio
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performLookup();
      return;
    }
    const int last = static_cast<int>(pageWords.size()) - 1;
    buttonNavigator.onNext([&] {
      if (wordIndex < last) {
        wordIndex++;
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([&] {
      if (wordIndex > 0) {
        wordIndex--;
        requestUpdate();
      }
    });
    return;
  }

  // Atrás mantenido (1 s) = voz, si el dueño lo pidió. Va ANTES de la suelta
  // corta: la misma pulsación termina soltando, y esa suelta ya no es "cerrar".
  if (onVoiceHold && mappedInput.wasLongPressed(MappedInputManager::Button::Back, 1000)) {
    onVoiceHold();
    leaving = true;
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leaving = true;
    finish();
    return;
  }
  // OK abre el menú, igual que en el lector de CrossPoint.
  if (!menuItems.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openMenu();
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

  if (mode == Mode::Words) {
    drawWordHighlight(fontId);
    const auto wordLabels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), (wordIndex > 0 ? tr(STR_DIR_UP) : ""),
                              (wordIndex + 1 < static_cast<int>(pageWords.size()) ? tr(STR_DIR_DOWN) : ""));
    GUI.drawButtonHints(renderer, wordLabels.btn1, wordLabels.btn2, wordLabels.btn3, wordLabels.btn4);
    if (menu.processRender(renderer, mappedInput)) return;
    if (popup != Popup::None) {
      GUI.drawPopup(renderer, I18N.get(popupMsg));  // dibuja y refresca él
      return;
    }
    // Sin la pasada de grises: el cursor se mueve palabra por palabra y cada
    // movimiento pagaría los 160 ms de los dos planos para suavizar un texto
    // que ya se leyó. Al volver al texto se pide un refresco limpio.
    const bool clean = wordsNeedClean;
    wordsNeedClean = false;
    renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    return;
  }

  // Con el menú de OK encendido, abajo a la izquierda dice "Atrás" (que es lo
  // que hace un toque) y a la derecha el menú: la etiqueta del atajo de voz
  // dejaba a la vista lo que hace MANTENIDO y escondía lo que hace tocando.
  const bool hasMenu = !menuItems.empty();
  const char* backLabel = hasMenu || !voiceHoldLabel ? tr(STR_BACK) : voiceHoldLabel;
  const auto labels =
      mappedInput.mapLabels(backLabel, hasMenu ? tr(STR_TEXT_MENU_TITLE) : "", (currentPage > 0 ? tr(STR_DIR_UP) : ""),
                            (currentPage + 1 < totalPages ? tr(STR_DIR_DOWN) : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (menu.processRender(renderer, mappedInput)) return;
  if (popup != Popup::None) {
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  // Esta pantalla es para leer (capítulos de la Biblia, respuestas, noticias),
  // así que el texto sale por el pipeline de grises igual que en el lector: la
  // base en blanco y negro y encima las dos pasadas de suavizado. Solo se
  // vuelve a dibujar el cuerpo; el encabezado y los botones quedan de la base.
  GrayText::displayPage(renderer, partialCount, [&] { drawBody(fontId, x, bodyStartY); });
}
