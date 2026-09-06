#include "AgendaActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <HalClock.h>
#include <ServerClient.h>

#include <ctime>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "AGENDA";
constexpr int ROW_H = 44;
constexpr int SIDE = 20;
constexpr unsigned long MENU_HOLD_MS = 1200;

std::string dateOffset(int days) {
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) return "";
  now += static_cast<time_t>(days) * 86400;
  struct tm t;
  gmtime_r(&now, &t);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}
}  // namespace

void AgendaActivity::onEnter() {
  Activity::onEnter();
  level = SECTIONS;
  sectionIndex = 0;
  itemIndex = 0;
  rebuildSections();
  requestUpdate();
}

// Messages (only while there are unread ones), reminders, then every list.
void AgendaActivity::rebuildSections() {
  sections.clear();
  if (!HUB_STORE.messages.empty()) sections.push_back({MESSAGES, -1});
  sections.push_back({REMINDERS, -1});
  for (int i = 0; i < static_cast<int>(HUB_STORE.lists.size()); ++i) sections.push_back({LIST, i});
  if (sectionIndex >= sectionCount()) sectionIndex = sectionCount() - 1;
}

int AgendaActivity::itemCount() const {
  switch (current().kind) {
    case MESSAGES: return static_cast<int>(HUB_STORE.messages.size());
    case REMINDERS: return static_cast<int>(HUB_STORE.reminders.size());
    case LIST: return static_cast<int>(HUB_STORE.lists[current().listIndex].items.size());
  }
  return 0;
}

std::string AgendaActivity::sectionTitle(const int index) const {
  switch (sections[index].kind) {
    case MESSAGES: return tr(STR_HUB_MESSAGES);
    case REMINDERS: return tr(STR_HUB_REMINDERS);
    case LIST: return HUB_STORE.lists[sections[index].listIndex].name;
  }
  return "";
}

std::string AgendaActivity::itemText(const int index, std::string& detail) const {
  detail.clear();
  switch (current().kind) {
    case MESSAGES: {
      const HubStore::Message& m = HUB_STORE.messages[index];
      detail = m.from;
      return m.text;
    }
    case REMINDERS: {
      const HubStore::Reminder& r = HUB_STORE.reminders[index];
      detail = r.when;
      return r.title;
    }
    case LIST: return HUB_STORE.lists[current().listIndex].items[index].text;
  }
  return "";
}

// Local removal first (the screen must answer right away), then the server:
// delivered now if the network is up, queued on the SD otherwise and replayed
// by the next hub sync.
void AgendaActivity::tickCurrent() {
  if (itemCount() == 0) return;
  int id = 0;
  const char* kind = "item";
  switch (current().kind) {
    case MESSAGES:
      kind = "message";
      id = HUB_STORE.messages[itemIndex].id;
      HUB_STORE.removeMessage(id);
      break;
    case REMINDERS:
      kind = "reminder";
      id = HUB_STORE.reminders[itemIndex].id;
      HUB_STORE.removeReminder(id);
      break;
    case LIST:
      id = HUB_STORE.lists[current().listIndex].items[itemIndex].id;
      HUB_STORE.removeItem(id);
      break;
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
  if (current().kind == MESSAGES && HUB_STORE.messages.empty()) {
    level = SECTIONS;  // the section disappears with its last message
    rebuildSections();
  } else if (itemIndex >= itemCount() && itemIndex > 0) {
    itemIndex--;
  }
  requestUpdate();
}

void AgendaActivity::sendEdit(const char* action, const char* list, const char* dueDate) {
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "item";
    doc["id"] = menuItemId;
    doc["action"] = action;
    if (list) doc["list"] = list;
    if (dueDate) doc["dueDate"] = dueDate;
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "edit %s %d: %s", action, menuItemId, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/edit", body)));
}

void AgendaActivity::openItemMenu() {
  if (level != ITEMS || current().kind != LIST || itemCount() == 0) return;  // lists only; the rest just ticks
  menuItemId = HUB_STORE.lists[current().listIndex].items[itemIndex].id;
  menuStep = MAIN;
  menuOptions = {tr(STR_AGENDA_MOVE), tr(STR_AGENDA_DATE), tr(STR_AGENDA_DELETE)};
  menu.show(StrId::STR_AGENDA_ITEM_MENU, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
  requestUpdate();
}

void AgendaActivity::onMenuPick(const int index) {
  if (menuStep == MAIN) {
    if (index == 0) {
      menuStep = MOVE;
      menuOptions.clear();
      for (const HubStore::List& l : HUB_STORE.lists) menuOptions.push_back(l.name);
      menu.show(StrId::STR_AGENDA_MOVE, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
    } else if (index == 1) {
      menuStep = DATE;
      menuOptions = {tr(STR_AGENDA_DATE_TODAY), tr(STR_AGENDA_DATE_TOMORROW), tr(STR_AGENDA_DATE_NEXT_WEEK),
                     tr(STR_AGENDA_DATE_NONE)};
      menu.show(StrId::STR_AGENDA_DATE, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
    } else if (index == 2) {
      HUB_STORE.removeItem(menuItemId);
      HUB_STORE.saveToFile();
      sendEdit("delete", nullptr, nullptr);
      menuStep = NONE;
      if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
    } else {
      menuStep = NONE;
    }
  } else if (menuStep == MOVE) {
    if (index >= 0 && index < static_cast<int>(menuOptions.size())) {
      const std::string target = menuOptions[index];
      HUB_STORE.moveItem(menuItemId, target);
      HUB_STORE.saveToFile();
      sendEdit("move", target.c_str(), nullptr);
      if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
    }
    menuStep = NONE;
  } else if (menuStep == DATE) {
    if (index == 0) sendEdit("date", nullptr, dateOffset(0).c_str());
    else if (index == 1) sendEdit("date", nullptr, dateOffset(1).c_str());
    else if (index == 2) sendEdit("date", nullptr, dateOffset(7).c_str());
    else if (index == 3) sendEdit("date", nullptr, "");
    menuStep = NONE;
  }
  requestUpdate();
}

void AgendaActivity::loop() {
  if (menuStep != NONE) {
    if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!menu.isActive() && menuStep != NONE) {
        // Back on a popup: a sub-menu returns to the main one, the main one closes.
        if (menuStep == MAIN) {
          menuStep = NONE;
        } else {
          menuStep = NONE;
          openItemMenu();
        }
        requestUpdate();
      }
    }
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
    openItemMenu();
    return;
  }
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
                              level == SECTIONS || current().kind == REMINDERS ? tr(STR_HUB_NO_REMINDERS)
                                                                                : tr(STR_AGENDA_EMPTY));
  }
  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * ROW_H;
    const bool isSelected = i == selected;
    if (isSelected) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
    const bool ink = !isSelected;
    std::string detail;
    std::string text;
    if (level == SECTIONS) {
      int n = 0;
      switch (sections[i].kind) {
        case MESSAGES: n = static_cast<int>(HUB_STORE.messages.size()); break;
        case REMINDERS: n = static_cast<int>(HUB_STORE.reminders.size()); break;
        case LIST: n = static_cast<int>(HUB_STORE.lists[sections[i].listIndex].items.size()); break;
      }
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

  if (menuStep != NONE && menu.processRender(renderer, mappedInput)) return;
  const char* okLabel = level == SECTIONS ? tr(STR_SELECT) : current().kind == MESSAGES ? tr(STR_AGENDA_READ) : tr(STR_AGENDA_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), okLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
