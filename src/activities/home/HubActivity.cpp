#include "HubActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <ServerCredentialStore.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Icon.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
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
#include "activities/games/LuaAppsActivity.h"
#include "WeatherActivity.h"
#include "NotesActivity.h"
#include "TimerActivity.h"
#include "TranslatorActivity.h"
#include "VoiceActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "util/Shtc3.h"
#include "components/icons/hubIcons.h"
#include "components/icons/hubWidgetIcons.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"
#include "components/Selection.h"
#include "music/MusicPlayer.h"


namespace {
constexpr int SIDE = 24;        // único margen lateral, y todo cae en la grilla de 8
constexpr int STATUS_H = 48;    // barra de estado (hora en UI_14)
constexpr int MIN_TILE_H = 82;  // icono de 48 + su etiqueta: menos que esto la corta
constexpr int MAX_TILE_H = 100;
constexpr int TILE_LABEL_H = 26;    // banda BLANCA de la etiqueta, debajo del campo del ícono
constexpr int TILE_RADIUS = 10;
constexpr int SUMMARY_ROW_H = 36;   // recordatorio, música/libro
constexpr int SUMMARY_WEATHER_H = 64;
constexpr int SUMMARY_MIN_H = 210;  // lo que el sumario le reserva a la grilla (incluye el aire de abajo)
constexpr int SUMMARY_GAP = 8;      // entre el sumario y el marco de la grilla
constexpr int HINT_GAP = 26;        // aire entre el aviso del atajo de voz y la barra de botones
constexpr unsigned long SYNC_HOLD_MS = 1200;      // Back held this long = sync now
constexpr unsigned long TICK_MS = 15000;          // cada cuánto se mira si cambió algo de la barra
constexpr time_t SYNC_INTERVAL_S = 3 * 3600;      // cache older than this at entry = sync
                                                  // (3 h: lo que se carga desde /board tarda menos en llegar)
constexpr time_t SYNC_RETRY_S = 3600;             // after a failed attempt

// Alturas de las fuentes (el ascender, que es lo que devuelve getTextHeight):
// UI_14 28, UI_12 24, UI_10 20, SMALL 18. Se usan para alinear por la línea de
// base en vez de a ojo.
constexpr int H_UI14 = 28;
constexpr int H_UI12 = 24;
constexpr int H_UI10 = 20;
constexpr int H_SMALL = 18;


struct TileSpec {
  StrId label;
  const freeink::Icon* icon;
};

// El orden manda: tiene que coincidir con el enum Tile del .h.
const TileSpec TILES[] = {
    {StrId::STR_HUB_DAY, &icon_hub_day_48},
    {StrId::STR_HUB_READ, &icon_hub_read_48},         {StrId::STR_HUB_TALK, &icon_hub_ask_48},
    {StrId::STR_HUB_TRANSLATOR, &icon_hub_translator_48},
    {StrId::STR_HUB_REMINDERS, &icon_hub_reminders_48}, {StrId::STR_HUB_TIMER, &icon_hub_timer_48},
    {StrId::STR_HUB_NOTES, &icon_hub_notes_48},       {StrId::STR_HUB_BIBLE, &icon_hub_bible_48},
    {StrId::STR_HUB_MUSIC, &icon_hub_music_48},       {StrId::STR_HUB_NEWS, &icon_hub_news_48},
    {StrId::STR_HUB_GAMES, &icon_hub_games_48},
    {StrId::STR_WEATHER_TITLE, &icon_hub_weather_48},
    {StrId::STR_SETTINGS_TITLE, &icon_hub_settings_48},
};
static_assert(sizeof(TILES) / sizeof(TILES[0]) == 13, "TILES tiene que seguir a Tile");

// "4:12" para el renglón de lo que suena.
std::string minutesSeconds(const int seconds) {
  if (seconds <= 0) return "";
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
  return buf;
}
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
      // El mosaico abre DIRECTO las apps en Lua. Los doce juegos compilados se
      // sacaron: "los juegos que tenemos no sirven, vaciá ese menú, vamos a
      // dejar ese espacio para que abra todo lo lua". Ya no hay un menú de
      // juegos con "Apps de la tarjeta" adentro: es Lua y nada más.
      activityManager.replaceActivity(std::make_unique<LuaAppsActivity>(renderer, mappedInput));
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
      break;
  }
}

void HubActivity::loop() {
  if (autoSyncPending && firstRenderDone) {
    autoSyncPending = false;
    startSync();
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
  // abre desde el mosaico Leer; antes Atrás lo abría y no había manera de
  // quedarse en el hub.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return;

  // La hora, el temporizador y lo que suena se repintan solos mientras el hub
  // está a la vista: un parcial por cambio y nada más (el panel quiere pocos).
  // Lo que suena (temporizador vencido, recordatorio en hora) lo dispara
  // checkTimeAlarms() en el loop de main.cpp, desde cualquier pantalla tranquila.
  const unsigned long now = millis();
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    char sig[sizeof(lastSignature)] = "";
    screenSignature(sig, sizeof(sig));
    if (strcmp(sig, lastSignature) != 0) requestUpdate();
  }
}

// Todo lo que se repinta solo mientras el hub está a la vista: el minuto, el
// temporizador y la pista que suena. Si no cambió nada, no se toca el panel.
void HubActivity::screenSignature(char* out, const size_t size) const {
  char clock[9] = "--:--";
  if (halClock.isAvailable()) {
    char buf[9] = {0};
    if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      snprintf(clock, sizeof(clock), "%s", buf);
    }
  }
  char chip[40] = "";
  formatTimeChip(chip, sizeof(chip));
  const std::string media = MUSIC.isActive() ? MUSIC.nowPlayingLine() : std::string();
  snprintf(out, size, "%s|%s|%d|%s", clock, chip, MUSIC.isPaused() ? 1 : 0, media.c_str());
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

std::vector<std::string> HubActivity::dateCandidates() const {
  std::vector<std::string> out;
  time_t now = 0;
  if (!halClock.getEpochUtc(now) || now <= 0) return out;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
  CalendarActivity::localFromEpoch(now, year, month, day, hour, minute);
  const int dow = CalendarActivity::weekdayOfCivil(year, month, day);
  const std::string dayNum = std::to_string(day);
  out.emplace_back(std::string(CalendarActivity::weekdayName(dow)) + " " + dayNum + " " +
                   CalendarActivity::monthName(month));
  out.emplace_back(std::string(CalendarActivity::weekdayShort(dow)) + " " + dayNum + " " +
                   CalendarActivity::monthName(month));
  // Último recurso numérico: entra hasta con el cronómetro andando y el WiFi
  // arriba, que es cuando el hueco de la fecha se queda en 60 px, y entra en
  // los siete idiomas (el mes en letras no entra en ruso ni en francés).
  out.emplace_back(std::string(CalendarActivity::weekdayShort(dow)) + " " + dayNum + "/" + std::to_string(month));
  return out;
}

// Hora grande a la izquierda con la fecha al lado, y a la derecha el
// temporizador, el WiFi y la batería. La hora va en UI_14 (alto de mayúscula
// 20 px contra 17 de UI_12): es el dato que más se mira y antes competía de
// igual a igual con todo lo demás de la barra.
void HubActivity::drawStatusLine(const int y, const int height) const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  char timeBuf[9] = {0};
  const char* clockText = "--:--";
  if (halClock.isAvailable() &&
      halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    clockText = timeBuf;
  }
  const int clockY = y + (height - H_UI14) / 2;
  const int baseline = clockY + H_UI14;  // todo lo demás de la barra se alinea acá
  renderer.drawText(UI_14_FONT_ID, SIDE, clockY, clockText, true, EpdFontFamily::BOLD);
  // Todo lo que se dibuja de derecha a izquierda tiene que frenar acá: si no,
  // con un texto largo (el chip del temporizador en ruso, por ejemplo) la x se
  // iba a negativo y el panel escupía "[GFX] !! Outside range" por cada píxel.
  const int dateX = SIDE + renderer.getTextWidth(UI_14_FONT_ID, clockText, EpdFontFamily::BOLD) + 14;

  // Battery (icon + percent), right. WiFi icon before it when a link is up.
  const int percentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int batteryX = std::max(dateX, renderer.getScreenWidth() - SIDE - metrics.batteryWidth - 4 - percentWidth);
  // drawBatteryLeft pone el porcentaje arriba de la Rect y el icono 6 px más
  // abajo, así que la Rect va donde arranca ese texto: con eso el porcentaje
  // comparte la línea de base con la hora.
  GUI.drawBatteryLeft(renderer,
                      Rect{batteryX, baseline - H_SMALL, metrics.batteryWidth, metrics.batteryHeight}, true);
  int rightEdge = batteryX - 12;
  if (WiFi.status() == WL_CONNECTED && rightEdge - 24 > dateX) {
    rightEdge -= 24;
    BaseTheme::drawIconBitmap(renderer, icon_wifi_24, rightEdge, y + (height - 24) / 2, true);
    rightEdge -= 12;
  }
  // Lo que esté corriendo en Tiempo se ve desde el hub (y suena desde acá
  // mientras el aparato está despierto). Va en su recuadro de 1 px para que se
  // lea como un dato aparte y no como una palabra suelta al lado de la fecha.
  {
    char t[40] = "";
    formatTimeChip(t, sizeof(t));
    if (t[0] && rightEdge - dateX > 60) {
      const std::string chip = renderer.truncatedText(UI_10_FONT_ID, t, rightEdge - dateX - 24, EpdFontFamily::BOLD);
      const int w = renderer.getTextWidth(UI_10_FONT_ID, chip.c_str(), EpdFontFamily::BOLD) + 20;
      // La caja tiene que darle lugar a las colas de las letras ("Tiempo"): con
      // la línea de base pegada al borde de abajo, la p sobresalía del recuadro.
      const int boxH = H_UI10 + 12;
      const int boxY = y + (height - boxH) / 2;
      rightEdge -= w;
      renderer.drawRoundedRect(rightEdge, boxY, w, boxH, 1, boxH / 2, true);
      renderer.drawText(UI_10_FONT_ID, rightEdge + 10, boxY + 6, chip.c_str(), true, EpdFontFamily::BOLD);
      rightEdge -= 12;
    }
  }

  // La fecha llena lo que quede entre la hora y lo de la derecha. NUNCA se
  // trunca: se prueban los candidatos de más largo a más corto y se dibuja el
  // primero que ENTRA de verdad. Con truncatedText, un temporizador andando
  // dejaba "Mi 9 Sep…" cortado a mitad de palabra y en ruso la fecha no
  // entraba nunca; si no entra ni el numérico, mejor nada que un muñón.
  const int dateW = rightEdge - dateX;
  for (const std::string& date : dateCandidates()) {
    if (renderer.getTextWidth(UI_10_FONT_ID, date.c_str()) > dateW) continue;
    renderer.drawText(UI_10_FONT_ID, dateX, baseline - H_UI10, date.c_str());
    break;
  }

  renderer.drawLine(SIDE, y + height, renderer.getScreenWidth() - SIDE, y + height, true);
}

// El sumario: clima, próximo recordatorio, lo que suena (o el libro abierto) y
// la agenda del día. Renglones separados por reglas de 1 px, sin marcos ni
// cajas. Si el tema deja menos alto del esperado se caen los de abajo, nunca el
// clima.
void HubActivity::drawSummary(const int x, const int y, const int w, const int h) const {
  int cursor = y;
  int left = h;
  auto rule = [&] {
    renderer.drawLine(x, cursor, x + w, cursor, true);
    cursor += 1;
    left -= 1;
  };

  const int weatherH = std::min(SUMMARY_WEATHER_H, left);
  drawWeatherRow(x, cursor, w, weatherH);
  cursor += weatherH;
  left -= weatherH;

  if (left >= SUMMARY_ROW_H + 1) {
    rule();
    drawReminderRow(x, cursor, w, SUMMARY_ROW_H);
    cursor += SUMMARY_ROW_H;
    left -= SUMMARY_ROW_H;
  }
  if (left >= SUMMARY_ROW_H + 1) {
    rule();
    drawMediaRow(x, cursor, w, SUMMARY_ROW_H);
    cursor += SUMMARY_ROW_H;
    left -= SUMMARY_ROW_H;
  }
  // La agenda se queda con lo que sobre: dos renglones si hay lugar, uno si no.
  if (left >= SUMMARY_ROW_H + 1) {
    rule();
    drawAgendaRow(x, cursor, w, left);
  }
}

// "Interior 23° · 45 %" del SHTC3 ("" si el sensor todavía no midió). Es el
// único dato de clima que el aparato tiene sin WiFi y sin servidor, así que se
// arma aparte: lo necesitan las DOS ramas del renglón del clima.
std::string HubActivity::indoorLine() const {
  const float inside = shtc3::cachedCelsius();
  if (std::isnan(inside)) return "";
  char in[48];
  const float rh = shtc3::cachedHumidity();
  if (!std::isnan(rh)) {
    snprintf(in, sizeof(in), "%s %d° · %d %%", tr(STR_HUB_INDOOR), static_cast<int>(inside + 0.5f),
             static_cast<int>(rh + 0.5f));
  } else {
    snprintf(in, sizeof(in), "%s %d°", tr(STR_HUB_INDOOR), static_cast<int>(inside + 0.5f));
  }
  return in;
}

// Clima: la temperatura es el número grande (UI_14) y el resto acompaña en
// UI_10, en vez de la línea corrida de antes donde "23°" pesaba lo mismo que
// "Algo nublado". La de afuera viene del servidor; la de adentro, del SHTC3.
void HubActivity::drawWeatherRow(const int x, const int y, const int w, const int h) const {
  const HubStore& hub = HUB_STORE;
  const int tx = x + 24 + 10;  // después del icono de 24
  const int tw = std::max(20, x + w - tx);
  BaseTheme::drawIconBitmap(renderer, icon_hub_weather_24, x, y + 4, true);

  if (hub.weatherLine.empty()) {
    // El motivo concreto en vez de "sin datos": falta el token, falta el lugar,
    // o el servicio del clima falló en el servidor.
    const char* none = !SERVER_STORE.hasToken()      ? tr(STR_HUB_NO_TOKEN)
                       : !hub.hasSynced()            ? tr(STR_HUB_NEVER_SYNCED)
                       : hub.weatherNoPlace          ? tr(STR_HUB_NO_PLACE)
                       : !hub.weatherError.empty()   ? tr(STR_HUB_WEATHER_ERROR)
                                                     : tr(STR_HUB_NO_WEATHER);
    // La interior va IGUAL, y acá es cuando más sirve: sin sincronizar, sin
    // lugar cargado o con el servicio de clima caído, es el único dato de
    // temperatura que tiene el aparato y antes desaparecía justo ahí.
    std::string indoor = indoorLine();
    int indoorW = indoor.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, indoor.c_str());
    if (indoorW > 0 && indoorW > tw - 80) {
      indoor.clear();  // no hay lugar: se cae antes que apretar el motivo
      indoorW = 0;
    }
    if (indoorW > 0) renderer.drawText(SMALL_FONT_ID, x + w - indoorW, y + 2 + H_UI10 - H_SMALL, indoor.c_str());
    const int noneW = std::max(20, tw - (indoorW ? indoorW + 12 : 0));
    int ly = y + 2;
    for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, none, noneW, 2)) {
      renderer.drawText(UI_10_FONT_ID, tx, ly, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), noneW).c_str());
      ly += H_UI10 + 4;
    }
    return;
  }

  // El servidor manda "<descripción> · <temperatura>°": se parte por el último
  // separador para poder darle a cada parte el peso que le toca.
  std::string condition = hub.weatherLine;
  std::string temperature;
  const size_t cut = condition.rfind(" \xC2\xB7 ");  // el separador es un punto medio UTF-8
  if (cut != std::string::npos) {
    const std::string tail = condition.substr(cut + 4);
    // La cola es la temperatura sólo si empieza con dígito o signo y termina en
    // grado ("\xC2\xB0"); con cualquier otra cosa se deja la línea entera.
    const bool degrees = tail.size() >= 3 && tail.compare(tail.size() - 2, 2, "\xC2\xB0") == 0;
    if (degrees && (isdigit(static_cast<unsigned char>(tail[0])) || tail[0] == '-')) {
      temperature = tail;
      condition = condition.substr(0, cut);
    }
  }

  const int baseline = y + 2 + H_UI14;
  int textX = tx;
  if (!temperature.empty()) {
    renderer.drawText(UI_14_FONT_ID, textX, y + 2, temperature.c_str(), true, EpdFontFamily::BOLD);
    textX += renderer.getTextWidth(UI_14_FONT_ID, temperature.c_str(), EpdFontFamily::BOLD) + 12;
  }

  // La interior (temperatura y humedad del SHTC3) va pegada al borde derecho.
  std::string indoor = indoorLine();
  int indoorW = indoor.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, indoor.c_str());
  if (indoorW > 0 && indoorW > x + w - textX - 40) {
    indoor.clear();  // no hay lugar: la interior se cae antes que cortar el titular
    indoorW = 0;
  }
  if (indoorW > 0) {
    renderer.drawText(SMALL_FONT_ID, x + w - indoorW, baseline - H_SMALL, indoor.c_str());
  }

  const int condW = std::max(20, x + w - textX - (indoorW ? indoorW + 12 : 0));
  renderer.drawText(UI_10_FONT_ID, textX, baseline - H_UI10,
                    renderer.truncatedText(UI_10_FONT_ID, condition.c_str(), condW, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);

  if (!hub.weatherDetail.empty() && h >= 56) {
    renderer.drawText(SMALL_FONT_ID, tx, y + h - H_SMALL - 6,
                      renderer.truncatedText(SMALL_FONT_ID, hub.weatherDetail.c_str(), tw).c_str());
  }
}

void HubActivity::drawReminderRow(const int x, const int y, const int w, const int h) const {
  const HubStore& hub = HUB_STORE;
  const int tx = x + 24 + 10;
  const int tw = std::max(20, x + w - tx);
  const int top = y + (h - H_UI12) / 2;
  BaseTheme::drawIconBitmap(renderer, icon_hub_reminder_24, x, y + (h - 24) / 2, true);
  if (hub.reminderTitle.empty()) {
    renderer.drawText(UI_10_FONT_ID, tx, y + (h - H_UI10) / 2,
                      renderer.truncatedText(UI_10_FONT_ID, tr(STR_HUB_NO_REMINDERS), tw).c_str());
    return;
  }
  const std::string when = renderer.truncatedText(SMALL_FONT_ID, hub.reminderWhen.c_str(), tw / 2);
  const int whenW = when.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, when.c_str());
  const int titleW = std::max(20, tw - (whenW ? whenW + 12 : 0));
  renderer.drawText(
      UI_12_FONT_ID, tx, top,
      renderer.truncatedText(UI_12_FONT_ID, hub.reminderTitle.c_str(), titleW, EpdFontFamily::BOLD).c_str(), true,
      EpdFontFamily::BOLD);
  if (whenW) renderer.drawText(SMALL_FONT_ID, x + w - whenW, top + H_UI12 - H_SMALL, when.c_str());
}

// Un solo renglón para lo que se está escuchando o leyendo: si suena algo, la
// pista (el libro sigue a un OK de distancia, en el mosaico Leer); si no suena
// nada, el libro abierto, que es lo que el usuario quiere ver siempre que haya
// uno empezado.
void HubActivity::drawMediaRow(const int x, const int y, const int w, const int h) const {
  const int tx = x + 24 + 10;
  const int tw = std::max(20, x + w - tx);
  const int top = y + (h - H_UI12) / 2;

  if (MUSIC.isActive()) {
    BaseTheme::drawIconBitmap(renderer, icon_hub_music_24, x, y + (h - 24) / 2, true);
    std::string side;
    if (MUSIC.isPaused()) {
      side = tr(STR_MUSIC_PAUSED);
    } else {
      const std::string length = minutesSeconds(MUSIC.durationSeconds());
      char buf[48];
      snprintf(buf, sizeof(buf), "%d / %d", MUSIC.index() + 1, MUSIC.count());
      side = buf;
      if (!length.empty()) side += " · " + length;
    }
    const int sideW = renderer.getTextWidth(SMALL_FONT_ID, side.c_str());
    renderer.drawText(SMALL_FONT_ID, x + w - sideW, top + H_UI12 - H_SMALL, side.c_str());
    renderer.drawText(
        UI_12_FONT_ID, tx, top,
        renderer.truncatedText(UI_12_FONT_ID, MUSIC.nowPlayingLine().c_str(), std::max(20, tw - sideW - 12),
                               EpdFontFamily::BOLD)
            .c_str(),
        true, EpdFontFamily::BOLD);
    return;
  }

  BaseTheme::drawIconBitmap(renderer, icon_book_24, x, y + (h - 24) / 2, true);
  if (lastBookPath.empty()) {
    renderer.drawText(UI_10_FONT_ID, tx, y + (h - H_UI10) / 2,
                      renderer.truncatedText(UI_10_FONT_ID, tr(STR_HUB_NO_BOOK), tw).c_str());
    return;
  }
  const std::string side = lastBookAuthor.empty() ? std::string(tr(STR_CONTINUE_READING)) : lastBookAuthor;
  const std::string shownSide = renderer.truncatedText(SMALL_FONT_ID, side.c_str(), tw / 2);
  const int sideW = renderer.getTextWidth(SMALL_FONT_ID, shownSide.c_str());
  renderer.drawText(SMALL_FONT_ID, x + w - sideW, top + H_UI12 - H_SMALL, shownSide.c_str());
  renderer.drawText(
      UI_12_FONT_ID, tx, top,
      renderer.truncatedText(UI_12_FONT_ID, lastBookTitle.c_str(), std::max(20, tw - sideW - 12),
                             EpdFontFamily::BOLD)
          .c_str(),
      true,
      EpdFontFamily::BOLD);
}

// La agenda de hoy o, con el día vacío, el versículo o la frase.
void HubActivity::drawAgendaRow(const int x, const int y, const int w, const int h) const {
  const HubStore& hub = HUB_STORE;
  const int tx = x + 24 + 10;
  const int tw = std::max(20, x + w - tx);
  const int step = H_UI10 + 4;
  const int maxLines = std::max(1, (h - 8) / step);
  // El icono va centrado sobre el PRIMER renglón, no sobre la caja entera.
  BaseTheme::drawIconBitmap(renderer, icon_hub_calendar_24, x, y + 4, true);
  int ly = y + 6;
  if (hub.events.empty()) {
    // No events: the verse of the day on even days, the quote on odd ones.
    time_t epoch = 0;
    const bool verseDay = halClock.getEpochUtc(epoch) && ((epoch / 86400) % 2 == 0);
    const bool useVerse = !hub.verseText.empty() && (verseDay || hub.quote.empty());
    const std::string line = useVerse      ? hub.verseText + " (" + hub.verseRef + ")"
                             : hub.quote.empty() ? std::string(tr(STR_HUB_NO_EVENTS))
                                                 : hub.quote;
    // Corte por palabras: truncar y seguir desde el corte partía la palabra al
    // medio y encima metía "…" en el medio de la frase.
    for (const std::string& part : renderer.wrappedText(UI_10_FONT_ID, line.c_str(), tw, maxLines)) {
      renderer.drawText(UI_10_FONT_ID, tx, ly, renderer.truncatedText(UI_10_FONT_ID, part.c_str(), tw).c_str());
      ly += step;
    }
    return;
  }
  int shown = 0;
  for (const HubStore::Event& e : hub.events) {
    if (shown++ >= maxLines) break;
    const std::string line = e.when.empty() ? e.title : e.when + "  " + e.title;
    renderer.drawText(UI_10_FONT_ID, tx, ly, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), tw).c_str());
    ly += step;
  }
}

// La grilla es una TABLA: un marco y reglas de 1 px entre celdas, no trece
// cajas sueltas con aire en el medio. "Mi día" ocupa la primera fila ENTERA:
// el Conversor, que le hacía compañía en la tercera columna, se sacó en 1.5.50
// ("no me es útil, eso prefiero preguntárselo a la IA en Hablar").
void HubActivity::drawGrid(const int x, const int y, const int w, const int h) const {
  const int cw = w / COLUMNS;
  const int ch = h / GRID_ROWS;
  const int gridW = cw * COLUMNS;
  const int gridH = ch * GRID_ROWS;
  renderer.drawRoundedRect(x, y, gridW, gridH, 1, TILE_RADIUS, true);
  for (int row = 1; row < GRID_ROWS; ++row) {
    renderer.drawLine(x, y + row * ch, x + gridW - 1, y + row * ch, true);
  }
  for (int row = 0; row < GRID_ROWS; ++row) {
    for (int col = 1; col < COLUMNS; ++col) {
      if (row == 0) continue;  // la primera fila es un solo mosaico ancho: no va raya
      renderer.drawLine(x + col * cw, y + row * ch, x + col * cw, y + (row + 1) * ch - 1, true);
    }
  }

  drawWideTile(TILE_DAY, x, y, gridW, ch);
  for (int i = TILE_DAY + 1; i < TILE_COUNT; ++i) {
    const int cell = i - (TILE_DAY + 1);
    drawTile(i, x + (cell % COLUMNS) * cw, y + (1 + cell / COLUMNS) * ch, cw, ch);
  }
}

// Mosaico: el resalte trama SOLO el campo del ícono y la etiqueta queda sobre
// blanco, debajo del borde de ese campo. Antes la trama tapaba la celda entera
// y la palabra sobre la que uno va a apretar OK era la menos legible de la
// pantalla. Debajo del ícono va una "moneda" blanca para que sus trazos de 2 px
// no se mezclen con los puntos de la trama.
void HubActivity::drawTile(const int index, const int x, const int y, const int w, const int h) const {
  const bool isSelected = index == selected;
  const TileSpec& spec = TILES[index];
  const int fieldH = std::max(spec.icon->h + 4, h - TILE_LABEL_H);
  const int iconX = x + (w - spec.icon->w) / 2;
  const int iconY = y + (fieldH - spec.icon->h) / 2;

  if (isSelected) {
    drawSelectionRow(renderer, x + 3, y + 3, w - 6, fieldH - 5, TILE_RADIUS, SelectionStyle::Tile);
    // Plato blanco: NUNCA hay trazos finos ni letras sobre trama.
    const int coinPadX = 12;
    const int coinPadY = std::max(0, (fieldH - 8 - spec.icon->h) / 2);
    // El marco del resalte ocupa las filas y+3 e y+4 (drawRoundedRect con
    // lineWidth 2 pinta hacia adentro): sin este tope la moneda blanca borraba
    // la fila de adentro y el borde de arriba del mosaico elegido quedaba de
    // 1 px en el tramo del medio, o sea justo en la celda que se está apuntando.
    const int coinTop = std::max(y + 6, iconY - coinPadY);
    const int coinBottom = iconY + spec.icon->h + coinPadY;
    renderer.fillRoundedRect(iconX - coinPadX, coinTop, spec.icon->w + 2 * coinPadX,
                             std::max(static_cast<int>(spec.icon->h), coinBottom - coinTop), 12, Color::White);
  }
  BaseTheme::drawIconBitmap(renderer, *spec.icon, iconX, iconY, SELECTION_INK);

  // Punto en la esquina cuando hay algo corriendo (temporizador) o sonando.
  if ((index == TILE_TIMER && HUB_STORE.timeActive()) || (index == TILE_MUSIC && MUSIC.isActive())) {
    renderer.fillRoundedRect(x + w - 20, y + 10, 8, 8, 4, Color::Black);
  }

  // La palabra larga se dibuja con la fuente chica antes que mutilarla:
  // "Recordatorios" entero, no "Recordato…". Truncar es el último recurso.
  const char* label = I18N.get(spec.label);
  const int textW = w - 10;
  int labelFont = UI_10_FONT_ID;
  EpdFontFamily::Style labelStyle = EpdFontFamily::BOLD;
  if (renderer.getTextWidth(labelFont, label, labelStyle) > textW) {
    labelFont = SMALL_FONT_ID;
    labelStyle = EpdFontFamily::BOLD;
  }
  const std::string fitted = renderer.truncatedText(labelFont, label, textW, labelStyle);
  const int labelH = renderer.getTextHeight(labelFont);
  const int labelW = renderer.getTextWidth(labelFont, fitted.c_str(), labelStyle);
  renderer.drawText(labelFont, x + (w - labelW) / 2, y + fieldH + (h - fieldH - labelH) / 2, fitted.c_str(),
                    SELECTION_INK, labelStyle);
}

// El mosaico ancho de la primera fila: icono a la izquierda, el nombre y un
// renglón que dice qué hay adentro. Lleva el resalte de FILA (no el de mosaico)
// porque su contenido es una fila de texto: el de mosaico tramaría la celda
// entera y las letras caerían encima.
void HubActivity::drawWideTile(const int index, const int x, const int y, const int w, const int h) const {
  const bool isSelected = index == selected;
  const TileSpec& spec = TILES[index];
  if (isSelected) drawSelectionRow(renderer, x + 3, y + 3, w - 6, h - 6, TILE_RADIUS, SelectionStyle::Row);
  // El icono arranca después de la franja tramada del resalte (pestaña + banda),
  // así nunca queda un trazo sobre los puntos.
  const int iconX = x + 6 + SELECTION_BAND_W + 8;
  BaseTheme::drawIconBitmap(renderer, *spec.icon, iconX, y + (h - spec.icon->h) / 2, SELECTION_INK);
  const int tx = iconX + spec.icon->w + 14;
  const int tw = std::max(20, x + w - tx - (6 + SELECTION_BAND_W + 4));
  // El subtítulo entra entero o no se dibuja: desde que este mosaico bajó de
  // tres columnas a dos le quedan 170 px, y truncarlo dejaba el PRIMER renglón
  // del hub diciendo "Hoy · Calenda…" en los siete idiomas. Sin subtítulo, el
  // nombre queda centrado y el mosaico se lee igual.
  const char* sub = tr(STR_HUB_DAY_SUB);
  const bool withSub = renderer.getTextWidth(SMALL_FONT_ID, sub) <= tw;
  const int block = withSub ? H_UI12 + 4 + H_SMALL : H_UI12;
  const int top = y + (h - block) / 2;
  renderer.drawText(UI_12_FONT_ID, tx, top,
                    renderer.truncatedText(UI_12_FONT_ID, I18N.get(spec.label), tw, EpdFontFamily::BOLD).c_str(),
                    SELECTION_INK, EpdFontFamily::BOLD);
  if (withSub) renderer.drawText(SMALL_FONT_ID, tx, top + H_UI12 + 4, sub, SELECTION_INK);
}

void HubActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentW = pageWidth - 2 * SIDE;

  renderer.clearScreen();
  drawStatusLine(metrics.topPadding, STATUS_H);

  // La grilla se queda con lo que sobra después del sumario, y el mosaico nunca
  // baja de MIN_TILE_H (lo que necesita el icono de 48 con su etiqueta adentro).
  const int summaryTop = metrics.topPadding + STATUS_H + 1;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int hintLine = hintsTop - HINT_GAP;
  const int gridBottom = hintLine - 12;
  const int available = gridBottom - summaryTop;
  const int tileH = std::min(MAX_TILE_H, std::max(MIN_TILE_H, (available - SUMMARY_MIN_H) / GRID_ROWS));
  const int gridH = tileH * GRID_ROWS;
  const int gridTop = gridBottom - gridH;
  const int summaryH = gridTop - SUMMARY_GAP - summaryTop;
  if (summaryH > SUMMARY_WEATHER_H) drawSummary(SIDE, summaryTop, contentW, summaryH);
  drawGrid(SIDE, gridTop, contentW, gridH);

  // Centrado, pero truncado contra el ancho útil: un texto más ancho que la
  // pantalla le deja a drawCenteredText una x negativa y se dibuja fuera del panel.
  renderer.drawCenteredText(SMALL_FONT_ID, hintLine,
                            renderer.truncatedText(SMALL_FONT_ID, tr(STR_VOICE_SHORTCUT_HINT), contentW).c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_HUB_SYNC_HINT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Lo que quedó dibujado de lo que cambia solo, para que el tick no repinte por gusto.
  screenSignature(lastSignature, sizeof(lastSignature));

  // La cadencia de limpiezas la lleva el coordinador del panel (una sola cuenta
  // para todo el aparato): acá sólo queda el limpio que pide quien nos abre
  // después de una Activity de red.
  const bool clean = cleanInitialRefresh && !firstRenderDone;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
