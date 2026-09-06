#include "AgendaActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "AGENDA";
constexpr int ROW_H = 44;
constexpr int SIDE = 20;
}  // namespace

void AgendaActivity::onEnter() {
  Activity::onEnter();
  level = SECTIONS;
  sectionIndex = 0;
  itemIndex = 0;
  requestUpdate();
}

int AgendaActivity::sectionCount() const { return 1 + static_cast<int>(HUB_STORE.lists.size()); }

int AgendaActivity::itemCount() const {
  if (sectionIndex == 0) return static_cast<int>(HUB_STORE.reminders.size());
  const int li = sectionIndex - 1;
  if (li < 0 || li >= static_cast<int>(HUB_STORE.lists.size())) return 0;
  return static_cast<int>(HUB_STORE.lists[li].items.size());
}

std::string AgendaActivity::sectionTitle(const int index) const {
  if (index == 0) return tr(STR_HUB_REMINDERS);
  return HUB_STORE.lists[index - 1].name;
}

std::string AgendaActivity::itemText(const int index, std::string& detail) const {
  detail.clear();
  if (sectionIndex == 0) {
    const HubStore::Reminder& r = HUB_STORE.reminders[index];
    detail = r.when;
    return r.title;
  }
  return HUB_STORE.lists[sectionIndex - 1].items[index].text;
}

// Local removal first (the screen must answer right away), then the server:
// delivered now if the network is up, queued on the SD otherwise and replayed
// by the next hub sync.
void AgendaActivity::tickCurrent() {
  if (itemCount() == 0) return;
  int id = 0;
  const char* kind = sectionIndex == 0 ? "reminder" : "item";
  if (sectionIndex == 0) {
    id = HUB_STORE.reminders[itemIndex].id;
    HUB_STORE.removeReminder(id);
  } else {
    id = HUB_STORE.lists[sectionIndex - 1].items[itemIndex].id;
    HUB_STORE.removeItem(id);
  }
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = kind;
    doc["id"] = id;
    serializeJson(doc, body);
  }
  const ServerClient::Result r = SERVER_CLIENT.postOrQueue("/api/hub/done", body);
  LOG_INF(TAG, "done %s %d: %s", kind, id, ServerClient::resultName(r));
  if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
  requestUpdate();
}

void AgendaActivity::loop() {
  const int count = level == SECTIONS ? sectionCount() : itemCount();
  int& index = level == SECTIONS ? sectionIndex : itemIndex;

  buttonNavigator.onNext([&] {
    if (count > 0) index = ButtonNavigator::nextIndex(index, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([&] {
    if (count > 0) index = ButtonNavigator::previousIndex(index, count);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (level == SECTIONS) {
      level = ITEMS;
      itemIndex = 0;
      requestUpdate();
    } else {
      tickCurrent();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (level == ITEMS) {
      level = SECTIONS;
      requestUpdate();
    } else {
      finish();
    }
  }
}

void AgendaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const std::string title = level == SECTIONS ? std::string(tr(STR_HUB_REMINDERS)) : sectionTitle(sectionIndex);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int top = metrics.topPadding + metrics.headerHeight + 12;
  const int bottom = pageHeight - metrics.buttonHintsHeight - 8;
  itemsPerPage = std::max(1, (bottom - top) / ROW_H);
  const int count = level == SECTIONS ? sectionCount() : itemCount();
  const int selected = level == SECTIONS ? sectionIndex : itemIndex;
  const int page = count > 0 ? selected / itemsPerPage : 0;
  const int first = page * itemsPerPage;

  if (count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10,
                              level == SECTIONS || sectionIndex == 0 ? tr(STR_HUB_NO_REMINDERS) : tr(STR_AGENDA_EMPTY));
  }
  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * ROW_H;
    const bool isSelected = i == selected;
    if (isSelected) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
    const bool ink = !isSelected;
    std::string detail;
    std::string text;
    if (level == SECTIONS) {
      const int n = i == 0 ? static_cast<int>(HUB_STORE.reminders.size())
                           : static_cast<int>(HUB_STORE.lists[i - 1].items.size());
      text = sectionTitle(i);
      detail = std::to_string(n);
    } else {
      text = itemText(i, detail);
    }
    const int detailW = detail.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, detail.c_str());
    const int textW = pageWidth - 2 * SIDE - detailW - (detailW ? 12 : 0);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 8, renderer.truncatedText(UI_12_FONT_ID, text.c_str(), textW).c_str(),
                      ink);
    if (detailW) renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - detailW, y + 11, detail.c_str(), ink);
  }
  if (count > itemsPerPage) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d/%d", page + 1, (count + itemsPerPage - 1) / itemsPerPage);
    renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom - 16, pages);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), level == SECTIONS ? tr(STR_SELECT) : tr(STR_AGENDA_DONE),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
