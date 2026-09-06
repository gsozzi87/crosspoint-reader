#include "NotesActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "NOTES";
constexpr int ROW_H = 60;
constexpr int SIDE = 20;
constexpr unsigned long MENU_HOLD_MS = 1200;
}  // namespace

void NotesActivity::onEnter() {
  Activity::onEnter();
  index = 0;
  requestUpdate();
}

void NotesActivity::openCurrent() {
  if (HUB_STORE.notes.empty()) return;
  const HubStore::Note& n = HUB_STORE.notes[index];
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, tr(STR_HUB_NOTES), n.text),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void NotesActivity::deleteCurrent() {
  if (HUB_STORE.notes.empty()) return;
  const int id = HUB_STORE.notes[index].id;
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
  if (index >= static_cast<int>(HUB_STORE.notes.size()) && index > 0) index--;
}

void NotesActivity::loop() {
  const int count = static_cast<int>(HUB_STORE.notes.size());
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
    if (count == 0) return;
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
    openCurrent();
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
  const int bottom = pageHeight - metrics.buttonHintsHeight - 8;
  perPage = std::max(1, (bottom - top) / ROW_H);
  const int count = static_cast<int>(HUB_STORE.notes.size());
  const int first = count > 0 ? (index / perPage) * perPage : 0;
  if (count == 0) renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_NOTES_EMPTY));
  for (int i = first; i < count && i < first + perPage; ++i) {
    const int y = top + (i - first) * ROW_H;
    const bool sel = i == index;
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
  if (confirming && confirm.processRender(renderer, mappedInput)) return;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
