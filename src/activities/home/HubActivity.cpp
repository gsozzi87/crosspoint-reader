#include "HubActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <ServerCredentialStore.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Icon.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "HubStore.h"
#include "HubSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "AgendaActivity.h"
#include "NotesActivity.h"
#include "TimerActivity.h"
#include "TranslatorActivity.h"
#include "ReminderAlertActivity.h"
#include "VoiceActivity.h"
#include "components/UITheme.h"
#include "components/icons/hubIcons.h"
#include "components/icons/hubWidgetIcons.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 20;        // left/right margin
constexpr int GAP = 12;         // between tiles
constexpr int STATUS_H = 44;    // status line band
constexpr int TILE_H = 104;
constexpr int TILE_RADIUS = 12;
constexpr int CONTINUE_H = 70;
constexpr int INFO_H = 150;
constexpr unsigned long SYNC_HOLD_MS = 1200;      // Back held this long = sync now
constexpr time_t SYNC_INTERVAL_S = 6 * 3600;      // cache older than this at entry = sync
constexpr time_t SYNC_RETRY_S = 3600;             // after a failed attempt

// Draws an SDK (freeink::Icon) bitmap through the renderer's pixel path, so it
// is orientation-correct and can be inverted for a selected (black) tile.
void drawSdkIcon(const GfxRenderer& renderer, const freeink::Icon& icon, int x, int y, bool black) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) == 0) renderer.drawPixel(x + col, y + row, black);
    }
  }
}

struct TileSpec {
  StrId label;
  const freeink::Icon* icon;
};

const TileSpec TILES[] = {
    {StrId::STR_HUB_READ, &icon_hub_read_48},           {StrId::STR_HUB_TALK, &icon_hub_ask_48},
    {StrId::STR_HUB_TRANSLATOR, &icon_hub_translator_48},
    {StrId::STR_HUB_REMINDERS, &icon_hub_reminders_48}, {StrId::STR_HUB_TIMER, &icon_hub_timer_48},
    {StrId::STR_HUB_NOTES, &icon_hub_notes_48},         {StrId::STR_HUB_BIBLE, &icon_hub_bible_48},
    {StrId::STR_HUB_MUSIC, &icon_hub_music_48},         {StrId::STR_SETTINGS_TITLE, &icon_hub_settings_48},
};
}  // namespace

void HubActivity::onEnter() {
  Activity::onEnter();
  loadLastBook();
  autoSyncPending = shouldAutoSync();
  requestUpdate();
}

// Sync on entry only when the cache is stale and we have not just tried: the
// hub is re-entered after every silent restart (Ask, Sync itself), and WiFi
// costs 10-20 s each time.
bool HubActivity::shouldAutoSync() const {
  if (!SERVER_STORE.hasToken()) return false;
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) {
    // No clock yet: only the very first run, so a dead server cannot loop us.
    return HUB_STORE.syncedAt == 0 && HUB_STORE.lastAttemptAt == 0;
  }
  if (HUB_STORE.syncedAt > 1 && now - HUB_STORE.syncedAt < SYNC_INTERVAL_S) return false;
  if (HUB_STORE.lastAttemptAt > 1 && now - HUB_STORE.lastAttemptAt < SYNC_RETRY_S) return false;
  return true;
}

void HubActivity::startSync() {
  activityManager.replaceActivity(std::make_unique<HubSyncActivity>(renderer, mappedInput));
}

void HubActivity::loadLastBook() {
  lastBookPath.clear();
  lastBookTitle.clear();
  lastBookAuthor.clear();
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    lastBookPath = book.path;
    lastBookTitle = book.title;
    lastBookAuthor = book.author;
    break;
  }
}

void HubActivity::activate(const int tile) {
  switch (tile) {
    case TILE_READ:
      activityManager.goToClassicHome();
      break;
    case TILE_TALK:
      activityManager.replaceActivity(std::make_unique<VoiceActivity>(renderer, mappedInput));
      break;
    case TILE_REMINDERS:
      startActivityForResult(std::make_unique<AgendaActivity>(renderer, mappedInput), [this](const ActivityResult&) {
        loadLastBook();
        requestUpdate();
      });
      break;
    case TILE_TRANSLATOR:
      activityManager.replaceActivity(std::make_unique<TranslatorActivity>(renderer, mappedInput));
      break;
    case TILE_TIMER:
      activityManager.pushActivity(std::make_unique<TimerActivity>(renderer, mappedInput));
      break;
    case TILE_NOTES:
      startActivityForResult(std::make_unique<NotesActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case TILE_SETTINGS:
      activityManager.goToSettings();
      break;
    default:
      comingSoon = true;
      requestUpdate();
      break;
  }
}

void HubActivity::loop() {
  if (autoSyncPending && firstRenderDone) {
    autoSyncPending = false;
    startSync();
    return;
  }
  if (comingSoon) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavNext) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
      comingSoon = false;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selected = ButtonNavigator::nextIndex(selected, TILE_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = ButtonNavigator::previousIndex(selected, TILE_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, SYNC_HOLD_MS)) {
    startSync();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !lastBookPath.empty()) {
    activityManager.goToReader(lastBookPath);
    return;
  }

  // Keep the clock honest while the hub sits on screen: one partial refresh
  // per minute change, nothing more (the panel wants few partials). The same
  // tick fires a reminder that came due while awake.
  const unsigned long now = millis();
  if (now - lastClockMinuteTick >= 15000) {
    lastClockMinuteTick = now;
    time_t epoch = 0;
    if (halClock.getEpochUtc(epoch)) {
      if (const HubStore::Reminder* due = HUB_STORE.dueReminder(epoch)) {
        startActivityForResult(
            std::make_unique<ReminderAlertActivity>(renderer, mappedInput, due->id, due->title, due->when),
            [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
    }
    char buf[9] = {0};
    if (halClock.isAvailable() &&
        halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1) &&
        strcmp(buf, lastClock) != 0) {
      requestUpdate();
    }
  }
}

void HubActivity::drawStatusLine(const int y, const int height) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int textY = y + (height - 24) / 2;

  // Clock, left. "--:--" until the RTC has been set (clock sync in Settings).
  char timeBuf[9] = {0};
  const char* clockText = "--:--";
  if (halClock.isAvailable() &&
      halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    clockText = timeBuf;
  }
  renderer.drawText(UI_12_FONT_ID, SIDE, textY, clockText, true, EpdFontFamily::BOLD);

  // Battery (icon + percent), right. WiFi icon before it when a link is up.
  const int percentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int batteryX = renderer.getScreenWidth() - SIDE - metrics.batteryWidth - 4 - percentWidth;
  GUI.drawBatteryLeft(renderer, Rect{batteryX, textY, metrics.batteryWidth, metrics.batteryHeight}, true);
  int rightEdge = batteryX - 12;
  if (WiFi.status() == WL_CONNECTED) {
    rightEdge -= 24;
    drawSdkIcon(renderer, icon_wifi_24, rightEdge, y + (height - 24) / 2, true);
    rightEdge -= 12;
  }
  if (!HUB_STORE.messages.empty()) {
    char count[24];
    snprintf(count, sizeof(count), tr(STR_HUB_MESSAGES_FORMAT), (int)HUB_STORE.messages.size());
    const int w = renderer.getTextWidth(UI_10_FONT_ID, count);
    rightEdge -= w;
    renderer.drawText(UI_10_FONT_ID, rightEdge, textY + 2, count);
    rightEdge -= 24 + 4;
    drawSdkIcon(renderer, icon_hub_message_24, rightEdge, y + (height - 24) / 2, true);
  }

  renderer.drawLine(SIDE, y + height, renderer.getScreenWidth() - SIDE, y + height, true);
}

void HubActivity::drawTile(const int index, const int x, const int y, const int w, const int h) const {
  const bool isSelected = index == selected && !comingSoon;
  const TileSpec& spec = TILES[index];
  if (isSelected) {
    renderer.fillRoundedRect(x, y, w, h, TILE_RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, TILE_RADIUS, true);
  }
  const bool ink = !isSelected;  // white on the selected tile
  const int iconX = x + (w - spec.icon->w) / 2;
  const int iconY = y + (h - spec.icon->h - 30) / 2;
  drawSdkIcon(renderer, *spec.icon, iconX, iconY, ink);

  const char* label = I18N.get(spec.label);
  const std::string shortLabel = renderer.truncatedText(UI_10_FONT_ID, label, w - 10, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, shortLabel.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x + (w - labelW) / 2, iconY + spec.icon->h + 6, shortLabel.c_str(), ink,
                    EpdFontFamily::BOLD);
}

void HubActivity::drawContinueWidget(const int x, const int y, const int w, const int h) const {
  renderer.drawRoundedRect(x, y, w, h, 1, TILE_RADIUS, true);
  drawSdkIcon(renderer, icon_book_24, x + 14, y + (h - 24) / 2, true);
  const int textX = x + 14 + 24 + 12;
  const int textW = w - (textX - x) - 14;
  if (lastBookPath.empty()) {
    renderer.drawText(UI_12_FONT_ID, textX, y + (h - 24) / 2, tr(STR_HUB_NO_BOOK), true, EpdFontFamily::BOLD);
    return;
  }
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, lastBookTitle.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, y + 8, title.c_str(), true, EpdFontFamily::BOLD);
  const std::string sub = lastBookAuthor.empty() ? tr(STR_CONTINUE_READING) : lastBookAuthor;
  renderer.drawText(UI_10_FONT_ID, textX, y + 34, renderer.truncatedText(UI_10_FONT_ID, sub.c_str(), textW).c_str());
}

// Weather + next reminder on the first row, today's events (or the quote) on
// the second. Everything comes from the SD cache; a never-synced hub says so.
void HubActivity::drawInfoWidgets(const int x, const int y, const int w, const int h) const {
  const HubStore& hub = HUB_STORE;
  renderer.drawRoundedRect(x, y, w, h, 1, TILE_RADIUS, true);
  const int colW = w / 2;
  const int pad = 14;
  const int rowH = 66;
  renderer.drawLine(x + colW, y + 10, x + colW, y + rowH - 4, true);
  renderer.drawLine(x + pad, y + rowH, x + w - pad, y + rowH, true);

  // Weather (left)
  {
    const int tx = x + pad + 24 + 8;
    const int tw = colW - pad - 24 - 8 - 8;
    drawSdkIcon(renderer, icon_hub_weather_24, x + pad, y + 12, true);
    if (hub.weatherLine.empty()) {
      renderer.drawText(UI_10_FONT_ID, tx, y + 14, renderer.truncatedText(UI_10_FONT_ID, hub.hasSynced() ? tr(STR_HUB_NO_WEATHER) : tr(STR_HUB_NEVER_SYNCED), tw).c_str());
    } else {
      renderer.drawText(UI_12_FONT_ID, tx, y + 8, renderer.truncatedText(UI_12_FONT_ID, hub.weatherLine.c_str(), tw, EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, tx, y + 38, renderer.truncatedText(SMALL_FONT_ID, hub.weatherDetail.c_str(), tw).c_str());
    }
  }
  // Next reminder (right)
  {
    const int ix = x + colW + pad;
    const int tx = ix + 24 + 8;
    const int tw = colW - pad - 24 - 8 - pad;
    drawSdkIcon(renderer, icon_hub_reminder_24, ix, y + 12, true);
    if (hub.reminderTitle.empty()) {
      renderer.drawText(UI_10_FONT_ID, tx, y + 14, renderer.truncatedText(UI_10_FONT_ID, tr(STR_HUB_NO_REMINDERS), tw).c_str());
    } else {
      renderer.drawText(UI_12_FONT_ID, tx, y + 8, renderer.truncatedText(UI_12_FONT_ID, hub.reminderTitle.c_str(), tw, EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, tx, y + 38, renderer.truncatedText(SMALL_FONT_ID, hub.reminderWhen.c_str(), tw).c_str());
    }
  }
  // Events, or the quote when the day is empty
  {
    const int ty = y + rowH + 10;
    const int tx = x + pad + 24 + 8;
    const int tw = w - (tx - x) - pad;
    drawSdkIcon(renderer, icon_hub_calendar_24, x + pad, ty, true);
    if (hub.events.empty()) {
      const std::string line = hub.quote.empty() ? std::string(tr(STR_HUB_NO_EVENTS)) : hub.quote;
      // Two lines max for the quote.
      const std::string first = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), tw);
      renderer.drawText(UI_10_FONT_ID, tx, ty + 2, first.c_str());
      if (first.size() + 3 < line.size() && line.compare(0, first.size() - 3, first, 0, first.size() - 3) == 0) {
        const std::string rest = line.substr(first.size() - 3);
        renderer.drawText(UI_10_FONT_ID, tx, ty + 28, renderer.truncatedText(UI_10_FONT_ID, rest.c_str(), tw).c_str());
      }
    } else {
      int ey = ty + 2;
      int shown = 0;
      for (const HubStore::Event& e : hub.events) {
        if (shown++ >= 3) break;
        std::string line = e.when.empty() ? e.title : e.when + "  " + e.title;
        renderer.drawText(UI_10_FONT_ID, tx, ey, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), tw).c_str());
        ey += 24;
      }
    }
  }
}

void HubActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  drawStatusLine(metrics.topPadding, STATUS_H);
  // Remember what the status line shows so loop() only repaints on a change.
  char timeBuf[9] = {0};
  if (halClock.isAvailable() &&
      halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    strncpy(lastClock, timeBuf, sizeof(lastClock));
  }

  const int tileW = (pageWidth - 2 * SIDE - GAP * (COLUMNS - 1)) / COLUMNS;
  const int rows = (TILE_COUNT + COLUMNS - 1) / COLUMNS;
  const int gridTop = metrics.topPadding + STATUS_H + 12;
  for (int i = 0; i < TILE_COUNT; ++i) {
    const int col = i % COLUMNS;
    const int row = i / COLUMNS;
    drawTile(i, SIDE + col * (tileW + GAP), gridTop + row * (TILE_H + GAP), tileW, TILE_H);
  }

  int widgetTop = gridTop + rows * TILE_H + (rows - 1) * GAP + 10;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  drawContinueWidget(SIDE, widgetTop, pageWidth - 2 * SIDE, CONTINUE_H);
  widgetTop += CONTINUE_H + 8;
  const int infoH = std::min(INFO_H, hintsTop - 8 - widgetTop);
  if (infoH > 80) drawInfoWidgets(SIDE, widgetTop, pageWidth - 2 * SIDE, infoH);

  if (comingSoon) GUI.drawPopup(renderer, tr(STR_HUB_COMING_SOON));

  const auto labels = mappedInput.mapLabels(lastBookPath.empty() ? tr(STR_HUB_SYNC_HINT) : tr(STR_RESUME), tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
