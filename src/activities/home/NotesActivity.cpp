#include "NotesActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "VoiceActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "NOTES";
constexpr int ROW_H = 60;
constexpr int ADD_ROW_H = 44;              // la fila de "dictar una nota", arriba de todo
constexpr int SIDE = 20;
constexpr int HINT_H = 20;                 // línea de ayuda arriba de la barra de botones
constexpr unsigned long MENU_HOLD_MS = 1200;
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
}  // namespace

int NotesActivity::rowCount() const { return 1 + static_cast<int>(HUB_STORE.notes.size()); }

int NotesActivity::noteIndex() const {
  const int i = index - 1;
  return i >= 0 && i < static_cast<int>(HUB_STORE.notes.size()) ? i : -1;
}

void NotesActivity::onEnter() {
  Activity::onEnter();
  index = 0;
  requestUpdate();
}

// El aparato no tiene teclado: una nota nueva se dicta. La fila de arriba lo
// dice con todas las letras en vez de dejarlo escondido en un atajo.
void NotesActivity::addByVoice() {
  activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInput));
}

void NotesActivity::openCurrent() {
  if (noteIndex() < 0) return;
  const HubStore::Note& n = HUB_STORE.notes[noteIndex()];
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, tr(STR_HUB_NOTES), n.text),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void NotesActivity::deleteCurrent() {
  if (noteIndex() < 0) return;
  const int id = HUB_STORE.notes[noteIndex()].id;
  HUB_STORE.removeNote(id);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "note";
    doc["id"] = id;
    doc["action"] = "delete";
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "delete %d: %s", id, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/edit", body)));
  if (index >= rowCount() && index > 0) index--;
}

void NotesActivity::loop() {
  const int count = rowCount();
  if (confirming) {
    if (confirm.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!confirm.isActive()) {
        confirming = false;
        requestUpdate();
      }
    }
    return;
  }
  buttonNavigator.onNext([&] {
    if (count > 0) index = ButtonNavigator::nextIndex(index, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([&] {
    if (count > 0) index = ButtonNavigator::previousIndex(index, count);
    requestUpdate();
  });
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
    if (noteIndex() < 0) return;  // la fila de "dictar" no se borra
    confirming = true;
    confirmOptions = {tr(STR_AGENDA_DELETE), tr(STR_BACK)};
    confirm.show(StrId::STR_HUB_NOTES, confirmOptions, 1, [this](int idx) {
      if (idx == 0) deleteCurrent();
      confirming = false;
      requestUpdate();
    });
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (index == 0) {
      addByVoice();
    } else {
      openCurrent();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void NotesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_NOTES));
  const int top = metrics.topPadding + metrics.headerHeight + 12;
  // El margen de abajo lleva verticalSpacing además del alto de los hints (misma
  // cuenta que SettingsActivity): con un 8 fijo la última fila quedaba pegada a la
  // barra de botones (verticalSpacing es 16 en Lyra, no 8).
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - HINT_H;
  const int notes = static_cast<int>(HUB_STORE.notes.size());

  // Fila 0: dictar una nota nueva. Siempre visible, no pagina.
  const bool addSelected = index == 0;
  if (addSelected) renderer.fillRoundedRect(SIDE - 6, top, pageWidth - 2 * (SIDE - 6), ADD_ROW_H - 4, 8, Color::Black);
  renderer.drawText(UI_12_FONT_ID, SIDE, top + 8,
                    renderer.truncatedText(UI_12_FONT_ID, tr(STR_NOTES_ADD), pageWidth - 2 * SIDE).c_str(),
                    !addSelected);

  const int listTop = top + ADD_ROW_H + 6;
  perPage = std::max(1, (bottom - listTop) / ROW_H);
  const int selectedNote = index - 1;  // -1 = la fila de dictar
  const int first = selectedNote > 0 ? (selectedNote / perPage) * perPage : 0;
  if (notes == 0) {
    int emptyY = listTop + 40;
    for (const std::string& line :
         renderer.wrappedText(UI_10_FONT_ID, tr(STR_NOTES_NONE_YET), pageWidth - 2 * SIDE, 2)) {
      renderer.drawCenteredText(UI_10_FONT_ID, emptyY, line.c_str());
      emptyY += 26;
    }
  }
  for (int i = first; i < notes && i < first + perPage; ++i) {
    const int y = listTop + (i - first) * ROW_H;
    const bool sel = i == selectedNote;
    if (sel) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
    const std::string& text = HUB_STORE.notes[i].text;
    const int w = pageWidth - 2 * SIDE;
    const std::string line1 = renderer.truncatedText(UI_12_FONT_ID, text.c_str(), w);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 6, line1.c_str(), !sel);
    // Second line: whatever did not fit on the first (the helper ends a cut with "...").
    if (line1.size() >= 3 && line1.size() < text.size() + 3 && text.compare(0, line1.size() - 3, line1, 0, line1.size() - 3) == 0) {
      const std::string rest = text.substr(line1.size() - 3);
      if (!rest.empty()) renderer.drawText(UI_10_FONT_ID, SIDE, y + 32, renderer.truncatedText(UI_10_FONT_ID, rest.c_str(), w).c_str(), !sel);
    }
  }
  // Cómo se borra: hasta ahora no lo decía nadie y el Atrás mantenido quedaba
  // escondido. Sobre la fila de dictar se muestra el atajo de voz.
  renderer.drawCenteredText(
      SMALL_FONT_ID, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - HINT_H + 2,
      renderer
          .truncatedText(SMALL_FONT_ID, index == 0 ? tr(STR_VOICE_SHORTCUT_HINT) : tr(STR_NOTES_DELETE_HINT),
                         pageWidth - 2 * SIDE)
          .c_str());

  if (confirming && confirm.processRender(renderer, mappedInput)) return;
  // La etiqueta del botón dice qué va a pasar: dictar arriba, abrir en una nota.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), index == 0 ? tr(STR_HUB_TALK) : tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Regla del panel: refresco limpio cada 10-15 parciales o la pantalla fantasmea.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
