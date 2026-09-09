#include "HubActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <ServerCredentialStore.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Icon.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HubStore.h"
#include "HubSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "AgendaActivity.h"
#include "CalendarActivity.h"
#include "BibleActivity.h"
#include "MusicActivity.h"
#include "NewsActivity.h"
#include "activities/games/GamesActivity.h"
#include "WeatherActivity.h"
#include "NotesActivity.h"
#include "TimerActivity.h"
#include "TranslatorActivity.h"
#include "VoiceActivity.h"
#include "components/UITheme.h"
#include "util/Shtc3.h"
#include "components/icons/hubIcons.h"
#include "components/icons/hubWidgetIcons.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 20;        // left/right margin
constexpr int GAP = 12;         // between tiles
constexpr int STATUS_H = 44;    // status line band
constexpr int WIDE_H = 56;      // el mosaico ancho de "Mi día", arriba de la grilla
constexpr int MIN_TILE_H = 82;  // icono de 48 + 6 + un renglón: menos que esto corta la etiqueta
constexpr int MAX_TILE_H = 100;
constexpr int TILE_RADIUS = 12;
constexpr int CONTINUE_H = 48;
constexpr int INFO_H = 130;     // bajó de 148: lo que le sacamos se lo lleva la fila del mosaico ancho
constexpr int HINT_GAP = 30;    // aire entre el aviso del atajo de voz y la barra de botones
constexpr unsigned long SYNC_HOLD_MS = 1200;      // Back held this long = sync now
constexpr int PARTIALS_BEFORE_CLEAN = 12;         // regla del panel: completo cada 10-15 parciales
constexpr time_t SYNC_INTERVAL_S = 3 * 3600;      // cache older than this at entry = sync
                                                  // (3 h: lo que se carga desde /board tarda menos en llegar)
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

// El orden manda: tiene que coincidir con el enum Tile del .h. El primero es el
// mosaico ancho; los otros doce van en la grilla de 3 columnas con iconos de 48.
const TileSpec TILES[] = {
    {StrId::STR_HUB_DAY, &icon_hub_day_48},
    {StrId::STR_HUB_READ, &icon_hub_read_48},           {StrId::STR_HUB_TALK, &icon_hub_ask_48},
    {StrId::STR_HUB_TRANSLATOR, &icon_hub_translator_48},
    {StrId::STR_HUB_REMINDERS, &icon_hub_reminders_48}, {StrId::STR_HUB_TIMER, &icon_hub_timer_48},
    {StrId::STR_HUB_NOTES, &icon_hub_notes_48},         {StrId::STR_HUB_BIBLE, &icon_hub_bible_48},
    {StrId::STR_HUB_MUSIC, &icon_hub_music_48},         {StrId::STR_HUB_NEWS, &icon_hub_news_48},
    {StrId::STR_HUB_GAMES, &icon_hub_games_48},         {StrId::STR_WEATHER_TITLE, &icon_hub_weather_48},
    {StrId::STR_SETTINGS_TITLE, &icon_hub_settings_48},
};
static_assert(sizeof(TILES) / sizeof(TILES[0]) == 13, "TILES tiene que seguir a Tile");
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
  // Clima vacío = falta el lugar o el servidor falló: reintentar a la hora en vez
  // de esperar el ciclo entero (el lugar recién cargado en /board entra acá).
  const time_t interval = HUB_STORE.weatherLine.empty() ? SYNC_RETRY_S : SYNC_INTERVAL_S;
  if (HUB_STORE.syncedAt > 1 && now - HUB_STORE.syncedAt < interval) return false;
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
    case TILE_DAY:
      startActivityForResult(std::make_unique<CalendarActivity>(renderer, mappedInput), [this](const ActivityResult&) {
        loadLastBook();
        requestUpdate();
      });
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
    case TILE_BIBLE:
      activityManager.replaceActivity(std::make_unique<BibleActivity>(renderer, mappedInput));
      break;
    case TILE_MUSIC:
      activityManager.replaceActivity(std::make_unique<MusicActivity>(renderer, mappedInput));
      break;
    case TILE_NEWS:
      activityManager.replaceActivity(std::make_unique<NewsActivity>(renderer, mappedInput));
      break;
    case TILE_WEATHER:
      activityManager.replaceActivity(std::make_unique<WeatherActivity>(renderer, mappedInput));
      break;
    case TILE_GAMES:
      activityManager.replaceActivity(std::make_unique<GamesActivity>(renderer, mappedInput));
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
  // El hub es el fondo del todo: Atrás no va a ninguna parte. El último libro se
  // abre con el widget "Continuar leyendo" (OK sobre el mosaico Leer) o desde
  // Leer; antes Atrás lo abría y no había manera de quedarse en el hub.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return;

  // Keep the clock honest while the hub sits on screen: one partial refresh
  // per minute change, nothing more (the panel wants few partials). Lo que suena
  // (temporizador vencido, recordatorio en hora) lo dispara checkTimeAlarms()
  // en el loop de main.cpp, que funciona desde cualquier pantalla tranquila.
  const unsigned long now = millis();
  if (now - lastClockMinuteTick >= 15000) {
    lastClockMinuteTick = now;
    time_t epoch = 0;
    if (halClock.getEpochUtc(epoch)) {
      // Repintar solo cuando cambia lo que se muestra (minutos), no cada tick.
      char chip[40] = "";
      formatTimeChip(chip, sizeof(chip));
      if (strcmp(chip, lastTimeChip) != 0) {
        snprintf(lastTimeChip, sizeof(lastTimeChip), "%s", chip);
        requestUpdate();
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

// En minutos, no en segundos: el hub repinta poco (parciales del panel), así
// que un M:SS quedaría siempre atrasado y encima fantasmearía la pantalla.
void HubActivity::formatTimeChip(char* out, const size_t size) const {
  out[0] = '\0';
  time_t now = 0;
  const bool haveClock = halClock.getEpochUtc(now);
  auto minutes = [](long seconds) { return seconds <= 0 ? 0L : (seconds + 59) / 60; };
  if (HUB_STORE.timerRunning() && haveClock) {
    const long left = static_cast<long>(HUB_STORE.timerEndAt - now);
    if (left > 60) snprintf(out, size, "%s %ld min", tr(STR_HUB_TIMER), minutes(left));
    else snprintf(out, size, "%s <1 min", tr(STR_HUB_TIMER));
  } else if (HUB_STORE.timerPaused()) {
    snprintf(out, size, "%s %ld min %s", tr(STR_HUB_TIMER), minutes(HUB_STORE.timerPausedLeft),
             tr(STR_TIMER_PAUSED_SHORT));
  } else if (HUB_STORE.stopwatchActive() && haveClock) {
    const long el = HUB_STORE.stopwatchElapsed(now);
    if (el >= 60) snprintf(out, size, "%s %ld min", tr(STR_TIMER_STOPWATCH), el / 60);
    else snprintf(out, size, "%s <1 min", tr(STR_TIMER_STOPWATCH));
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
  // Todo lo que se dibuja de derecha a izquierda tiene que frenar acá: si no,
  // con un texto largo (el chip del temporizador en ruso, por ejemplo) la x se
  // iba a negativo y el panel escupía "[GFX] !! Outside range" por cada píxel.
  const int leftLimit = SIDE + renderer.getTextWidth(UI_12_FONT_ID, clockText, EpdFontFamily::BOLD) + 14;

  // Battery (icon + percent), right. WiFi icon before it when a link is up.
  const int percentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int batteryX =
      std::max(leftLimit, renderer.getScreenWidth() - SIDE - metrics.batteryWidth - 4 - percentWidth);
  GUI.drawBatteryLeft(renderer, Rect{batteryX, textY, metrics.batteryWidth, metrics.batteryHeight}, true);
  int rightEdge = batteryX - 12;
  if (WiFi.status() == WL_CONNECTED && rightEdge - 24 > leftLimit) {
    rightEdge -= 24;
    drawSdkIcon(renderer, icon_wifi_24, rightEdge, y + (height - 24) / 2, true);
    rightEdge -= 12;
  }
  // Lo que esté corriendo en Tiempo se ve desde el hub (y suena desde acá
  // mientras el aparato está despierto).
  {
    char t[40] = "";
    formatTimeChip(t, sizeof(t));
    if (t[0] && rightEdge - leftLimit > 30) {
      const std::string chip = renderer.truncatedText(UI_10_FONT_ID, t, rightEdge - leftLimit, EpdFontFamily::BOLD);
      const int w = renderer.getTextWidth(UI_10_FONT_ID, chip.c_str(), EpdFontFamily::BOLD);
      rightEdge -= w;
      renderer.drawText(UI_10_FONT_ID, rightEdge, textY + 2, chip.c_str(), true, EpdFontFamily::BOLD);
      rightEdge -= 12;
    }
  }
  if (!HUB_STORE.messages.empty() && rightEdge - leftLimit > 40) {
    char count[24];
    snprintf(count, sizeof(count), tr(STR_HUB_MESSAGES_FORMAT), (int)HUB_STORE.messages.size());
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, count, rightEdge - leftLimit - 28);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, shown.c_str());
    rightEdge -= w;
    renderer.drawText(UI_10_FONT_ID, rightEdge, textY + 2, shown.c_str());
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

  // Con el icono de 64 px no entran dos renglones de etiqueta (quedarían por
  // debajo del borde del mosaico), así que la palabra larga se dibuja con la
  // fuente chica en vez de mutilarla: "Recordatorios" entero, no "Recordato…".
  // Truncar sigue siendo el último recurso.
  const char* label = I18N.get(spec.label);
  const int textW = w - 8;
  int labelFont = UI_10_FONT_ID;
  EpdFontFamily::Style labelStyle = EpdFontFamily::BOLD;
  if (renderer.getTextWidth(labelFont, label, labelStyle) > textW) {
    labelFont = SMALL_FONT_ID;
    labelStyle = EpdFontFamily::REGULAR;
  }
  const std::string fitted = renderer.truncatedText(labelFont, label, textW, labelStyle);

  const int block = spec.icon->h + 6 + renderer.getTextHeight(labelFont);
  const int iconX = x + (w - spec.icon->w) / 2;
  const int iconY = y + std::max(2, (h - block) / 2);
  drawSdkIcon(renderer, *spec.icon, iconX, iconY, ink);

  // Punto en la esquina del mosaico Tiempo cuando hay algo corriendo o pausado.
  if (index == TILE_TIMER && HUB_STORE.timeActive()) {
    renderer.fillRoundedRect(x + w - 18, y + 10, 8, 8, 4, ink ? Color::Black : Color::White);
  }

  const int labelW = renderer.getTextWidth(labelFont, fitted.c_str(), labelStyle);
  renderer.drawText(labelFont, x + (w - labelW) / 2, iconY + spec.icon->h + 6, fitted.c_str(), ink, labelStyle);
}

// Mosaico ancho: una fila entera con el icono a la izquierda, el nombre y un
// renglón que dice qué hay adentro. Es lo que evita que el mosaico número 13
// deje una fila coja en una grilla de tres columnas.
void HubActivity::drawWideTile(const int index, const int x, const int y, const int w, const int h) const {
  const bool isSelected = index == selected && !comingSoon;
  const TileSpec& spec = TILES[index];
  if (isSelected) renderer.fillRoundedRect(x, y, w, h, TILE_RADIUS, Color::Black);
  else renderer.drawRoundedRect(x, y, w, h, 2, TILE_RADIUS, true);
  const bool ink = !isSelected;
  drawSdkIcon(renderer, *spec.icon, x + 16, y + (h - spec.icon->h) / 2, ink);
  const int tx = x + 16 + spec.icon->w + 14;
  const int tw = std::max(20, w - (tx - x) - 14);
  renderer.drawText(UI_12_FONT_ID, tx, y + 8,
                    renderer.truncatedText(UI_12_FONT_ID, I18N.get(spec.label), tw, EpdFontFamily::BOLD).c_str(), ink,
                    EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, tx, y + 32,
                    renderer.truncatedText(SMALL_FONT_ID, tr(STR_HUB_DAY_SUB), tw).c_str(), ink);
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
  renderer.drawText(UI_12_FONT_ID, textX, y + 4, title.c_str(), true, EpdFontFamily::BOLD);
  const std::string sub = lastBookAuthor.empty() ? tr(STR_CONTINUE_READING) : lastBookAuthor;
  renderer.drawText(SMALL_FONT_ID, textX, y + 28, renderer.truncatedText(SMALL_FONT_ID, sub.c_str(), textW).c_str());
}

// Clima arriba (fila entera), después el próximo recordatorio y, al final, la
// agenda de hoy o la frase del día. Todo sale de la caché de la SD; un hub que
// nunca sincronizó lo dice. El clima ocupa el ancho completo: en media caja la
// línea del lugar se cortaba en "Iztac…" y no se alcanzaba a leer.
void HubActivity::drawInfoWidgets(const int x, const int y, const int w, const int h) const {
  const HubStore& hub = HUB_STORE;
  renderer.drawRoundedRect(x, y, w, h, 1, TILE_RADIUS, true);
  const int pad = 14;
  const int tx = x + pad + 24 + 8;  // el texto arranca después del icono de 24
  const int tw = std::max(20, w - (tx - x) - pad);
  const int lineStep = renderer.getTextHeight(UI_10_FONT_ID) + 4;
  const int weatherH = 56;

  // Clima (fila entera, dos renglones)
  {
    drawSdkIcon(renderer, icon_hub_weather_24, x + pad, y + 8, true);
    if (hub.weatherLine.empty()) {
      // El motivo concreto en vez de "sin datos": falta el token, falta el lugar,
      // o el servicio del clima falló en el servidor.
      const char* none = !SERVER_STORE.hasToken() ? tr(STR_HUB_NO_TOKEN)
                         : !hub.hasSynced()       ? tr(STR_HUB_NEVER_SYNCED)
                         : hub.weatherNoPlace     ? tr(STR_HUB_NO_PLACE)
                         : !hub.weatherError.empty() ? tr(STR_HUB_WEATHER_ERROR)
                                                     : tr(STR_HUB_NO_WEATHER);
      int ly = y + 8;
      for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, none, tw, 2)) {
        renderer.drawText(UI_10_FONT_ID, tx, ly, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), tw).c_str());
        ly += lineStep;
      }
    } else {
      // La de afuera viene del servidor; la de adentro, del SHTC3 de la placa.
      std::string indoor;
      const float inside = shtc3::cachedCelsius();
      if (!std::isnan(inside)) {
        char in[32];
        snprintf(in, sizeof(in), "%s %d°", tr(STR_HUB_INDOOR), static_cast<int>(inside + 0.5f));
        indoor = in;
      }
      const std::string title =
          renderer.truncatedText(UI_12_FONT_ID, hub.weatherLine.c_str(), tw, EpdFontFamily::BOLD);
      const int titleW = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
      std::string detail = hub.weatherDetail;
      if (!indoor.empty()) {
        // La interior va al lado del titular si sobra lugar; si no, adelante del
        // detalle, nunca cortada.
        if (titleW + 10 + renderer.getTextWidth(SMALL_FONT_ID, indoor.c_str()) <= tw) {
          renderer.drawText(SMALL_FONT_ID, tx + titleW + 10, y + 10, indoor.c_str());
        } else {
          detail = detail.empty() ? indoor : indoor + "  ·  " + detail;
        }
      }
      renderer.drawText(UI_12_FONT_ID, tx, y + 4, title.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, tx, y + 32,
                        renderer.truncatedText(SMALL_FONT_ID, detail.c_str(), tw).c_str());
    }
  }
  renderer.drawLine(x + pad, y + weatherH, x + w - pad, y + weatherH, true);

  // Próximo recordatorio (una fila entera: título y cuándo, cada uno truncado)
  const int ry = y + weatherH + 6;
  {
    drawSdkIcon(renderer, icon_hub_reminder_24, x + pad, ry, true);
    if (hub.reminderTitle.empty()) {
      renderer.drawText(UI_10_FONT_ID, tx, ry + 2,
                        renderer.truncatedText(UI_10_FONT_ID, tr(STR_HUB_NO_REMINDERS), tw).c_str());
    } else {
      const std::string when = renderer.truncatedText(SMALL_FONT_ID, hub.reminderWhen.c_str(), tw / 2);
      const int whenW = when.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, when.c_str());
      const int titleW = std::max(20, tw - (whenW ? whenW + 12 : 0));
      renderer.drawText(
          UI_12_FONT_ID, tx, ry,
          renderer.truncatedText(UI_12_FONT_ID, hub.reminderTitle.c_str(), titleW, EpdFontFamily::BOLD).c_str(), true,
          EpdFontFamily::BOLD);
      if (whenW) renderer.drawText(SMALL_FONT_ID, x + w - pad - whenW, ry + 6, when.c_str());
    }
  }

  // Events, or the quote when the day is empty
  {
    const int ty = ry + 30;
    // Cuántos renglones entran de verdad en lo que queda de la caja.
    const int maxLines = std::max(1, (y + h - 6 - ty) / lineStep);
    drawSdkIcon(renderer, icon_hub_calendar_24, x + pad, ty, true);
    if (hub.events.empty()) {
      // No events: the verse of the day on even days, the quote on odd ones.
      time_t epoch = 0;
      const bool verseDay = halClock.getEpochUtc(epoch) && ((epoch / 86400) % 2 == 0);
      const bool useVerse = !hub.verseText.empty() && (verseDay || hub.quote.empty());
      const std::string line = useVerse ? hub.verseText + " (" + hub.verseRef + ")"
                                        : hub.quote.empty() ? std::string(tr(STR_HUB_NO_EVENTS)) : hub.quote;
      // Corte por palabras, no por caracteres: el truco viejo (truncar y seguir
      // desde el corte) partía la palabra al medio y encima le metía "…" en el
      // medio de la frase. wrappedText corta por espacios y deja el "…" solo en
      // el último renglón que entra.
      int ey = ty;
      for (const std::string& part : renderer.wrappedText(UI_10_FONT_ID, line.c_str(), tw, maxLines)) {
        renderer.drawText(UI_10_FONT_ID, tx, ey, renderer.truncatedText(UI_10_FONT_ID, part.c_str(), tw).c_str());
        ey += lineStep;
      }
    } else {
      int ey = ty;
      int shown = 0;
      for (const HubStore::Event& e : hub.events) {
        if (shown++ >= maxLines) break;
        std::string line = e.when.empty() ? e.title : e.when + "  " + e.title;
        renderer.drawText(UI_10_FONT_ID, tx, ey, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), tw).c_str());
        ey += lineStep;
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

  // "Mi día" ancho arriba y los otros 12 en 3 columnas x 4 filas. El alto del
  // mosaico se calcula con lo que queda libre (los temas no tienen el mismo
  // topPadding ni la misma barra de botones) y nunca baja de MIN_TILE_H, que es
  // lo que necesita el icono de 48 con su etiqueta adentro del borde.
  const int gridTop = metrics.topPadding + STATUS_H + 8;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int hintLine = hintsTop - HINT_GAP;
  const int widgetsH = CONTINUE_H + 8 + INFO_H + 8;
  const int gridH = hintLine - 10 - widgetsH - gridTop;
  const int tileH =
      std::min(MAX_TILE_H, std::max(MIN_TILE_H, (gridH - WIDE_H - GAP - (GRID_ROWS - 1) * GAP) / GRID_ROWS));
  const int tileW = (pageWidth - 2 * SIDE - GAP * (COLUMNS - 1)) / COLUMNS;
  const int gridLeft = (pageWidth - (COLUMNS * tileW + (COLUMNS - 1) * GAP)) / 2;
  drawWideTile(TILE_DAY, SIDE, gridTop, pageWidth - 2 * SIDE, WIDE_H);
  const int tilesTop = gridTop + WIDE_H + GAP;
  for (int i = TILE_DAY + 1; i < TILE_COUNT; ++i) {
    const int cell = i - (TILE_DAY + 1);
    const int col = cell % COLUMNS;
    const int row = cell / COLUMNS;
    drawTile(i, gridLeft + col * (tileW + GAP), tilesTop + row * (tileH + GAP), tileW, tileH);
  }

  int widgetTop = tilesTop + GRID_ROWS * tileH + (GRID_ROWS - 1) * GAP + 8;
  drawContinueWidget(SIDE, widgetTop, pageWidth - 2 * SIDE, CONTINUE_H);
  widgetTop += CONTINUE_H + 8;
  // El aviso del atajo de voz se dibuja arriba de la barra de botones, así que
  // los widgets tienen que terminar antes o le pasan por encima (y él, a su vez,
  // tiene que terminar antes del borde de arriba de la barra).
  const int infoH = std::min(INFO_H, hintLine - 10 - widgetTop);
  if (infoH > 80) drawInfoWidgets(SIDE, widgetTop, pageWidth - 2 * SIDE, infoH);

  // Centrado, pero truncado contra el ancho útil: un texto más ancho que la
  // pantalla le deja a drawCenteredText una x negativa y se dibuja fuera del panel.
  renderer.drawCenteredText(
      SMALL_FONT_ID, hintLine,
      renderer.truncatedText(SMALL_FONT_ID, tr(STR_VOICE_SHORTCUT_HINT), pageWidth - 2 * SIDE).c_str());

  if (comingSoon) GUI.drawPopup(renderer, tr(STR_HUB_COMING_SOON));

  const auto labels = mappedInput.mapLabels(tr(STR_HUB_SYNC_HINT), tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool clean = (cleanInitialRefresh && !firstRenderDone) || partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  else ++partialCount;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
