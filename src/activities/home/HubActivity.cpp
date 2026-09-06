#include "HubActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Icon.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/AskBookActivity.h"
#include "components/UITheme.h"
#include "components/icons/hubIcons.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 20;        // left/right margin
constexpr int GAP = 16;         // between tiles
constexpr int STATUS_H = 44;    // status line band
constexpr int TILE_H = 140;
constexpr int TILE_RADIUS = 14;
constexpr int WIDGET_H = 96;

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
    {StrId::STR_HUB_READ, &icon_hub_read_48},         {StrId::STR_HUB_ASK, &icon_hub_ask_48},
    {StrId::STR_HUB_REMINDERS, &icon_hub_reminders_48}, {StrId::STR_HUB_BIBLE, &icon_hub_bible_48},
    {StrId::STR_HUB_MUSIC, &icon_hub_music_48},       {StrId::STR_SETTINGS_TITLE, &icon_hub_settings_48},
};
}  // namespace

void HubActivity::onEnter() {
  Activity::onEnter();
  loadLastBook();
  requestUpdate();
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
    case TILE_ASK:
      activityManager.replaceActivity(std::make_unique<AskBookActivity>(renderer, mappedInput));
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !lastBookPath.empty()) {
    activityManager.goToReader(lastBookPath);
    return;
  }

  // Keep the clock honest while the hub sits on screen: one partial refresh
  // per minute change, nothing more (the panel wants few partials).
  const unsigned long now = millis();
  if (now - lastClockMinuteTick >= 15000) {
    lastClockMinuteTick = now;
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
  if (WiFi.status() == WL_CONNECTED) {
    drawSdkIcon(renderer, icon_wifi_24, batteryX - 24 - 12, y + (height - 24) / 2, true);
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
  const int iconY = y + 26;
  drawSdkIcon(renderer, *spec.icon, iconX, iconY, ink);

  const char* label = I18N.get(spec.label);
  const std::string shortLabel = renderer.truncatedText(UI_12_FONT_ID, label, w - 16, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(UI_12_FONT_ID, shortLabel.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - labelW) / 2, iconY + spec.icon->h + 16, shortLabel.c_str(), ink,
                    EpdFontFamily::BOLD);
}

void HubActivity::drawContinueWidget(const int x, const int y, const int w, const int h) const {
  renderer.drawRoundedRect(x, y, w, h, 1, TILE_RADIUS, true);
  drawSdkIcon(renderer, icon_book_24, x + 14, y + 14, true);
  renderer.drawText(UI_10_FONT_ID, x + 14 + 24 + 10, y + 14, tr(STR_CONTINUE_READING));
  const int textW = w - 28;
  if (lastBookPath.empty()) {
    renderer.drawText(UI_12_FONT_ID, x + 14, y + 46, tr(STR_HUB_NO_BOOK), true, EpdFontFamily::BOLD);
    return;
  }
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, lastBookTitle.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + 14, y + 42, title.c_str(), true, EpdFontFamily::BOLD);
  if (!lastBookAuthor.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, x + 14, y + 68, author.c_str());
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
  const int gridTop = metrics.topPadding + STATUS_H + 18;
  for (int i = 0; i < TILE_COUNT; ++i) {
    const int col = i % COLUMNS;
    const int row = i / COLUMNS;
    drawTile(i, SIDE + col * (tileW + GAP), gridTop + row * (TILE_H + GAP), tileW, TILE_H);
  }

  const int widgetTop = gridTop + rows * TILE_H + (rows - 1) * GAP + 18;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int widgetH = std::min(WIDGET_H, hintsTop - 12 - widgetTop);
  if (widgetH > 40) drawContinueWidget(SIDE, widgetTop, pageWidth - 2 * SIDE, widgetH);

  if (comingSoon) GUI.drawPopup(renderer, tr(STR_HUB_COMING_SOON));

  const auto labels =
      mappedInput.mapLabels(lastBookPath.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
