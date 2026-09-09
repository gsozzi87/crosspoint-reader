#include "CalendarActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "TripActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"
#include "components/Selection.h"

namespace {
constexpr const char* TAG = "CAL";
constexpr const char* CACHE = "/.crosspoint/calendar.json";
constexpr int SIDE = 12;
constexpr int ROW_H = 56;                  // filas de la vista de día
constexpr unsigned long REFRESH_HOLD_MS = 1200;
constexpr unsigned long MENU_HOLD_MS = 1200;   // Atrás mantenido sobre una actividad
constexpr uint32_t DICTATE_TIMEOUT_MS = 90000;  // el modelo parte el día dictado
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
constexpr time_t CACHE_MAX_AGE_S = 6 * 3600;
constexpr int MAX_CACHED_MONTHS = 3;
constexpr int MAX_CACHED_DAYS = 40;
constexpr size_t MAX_MONTH_BODY = 96 * 1024;  // más que esto, se pide solo el resumen del mes

// La caché entera: {months:[{month,savedAt,days:[{d,c,t}]}], days:[{date,savedAt,items:[...]}]}
bool readCache(JsonDocument& doc) {
  if (!Storage.exists(CACHE)) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, CACHE, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = raw.empty() ? 0 : f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return false;
  return deserializeJson(doc, raw) == DeserializationError::Ok;
}

void writeCache(const JsonDocument& doc) {
  std::string raw;
  serializeJson(doc, raw);
  HalFile f;
  if (!Storage.openFileForWrite(TAG, CACHE, f)) return;
  f.write(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
  f.close();
}

std::string monthKey(const int year, const int month) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04d-%02d", year, month);
  return buf;
}

// El servidor manda la fecha como "YYYY-MM-DD"; el día es lo único que hace
// falta para la cuadrícula.
int dayOfIso(const std::string& iso) {
  if (iso.size() < 10) return 0;
  return (iso[8] - '0') * 10 + (iso[9] - '0');
}

// Un evento puede venir con `date`/`at` sueltos o con un `start` ISO completo:
// se aceptan las dos formas para no depender de un detalle del Hono.
//
// Se guarda TODO lo que el servidor manda de la actividad, no solo lo que se
// pinta: POST /api/calendar/event reemplaza el evento entero, así que para
// cambiarle la hora sin borrarle el lugar, la nota, el fin y la repetición hay
// que devolvérselos tal cual vinieron.
void readEvent(JsonVariantConst v, CalendarActivity::Item& out, std::string& date) {
  // `start` puede ser el arranque de la serie ("2026-09-15T10:30") o un epoch
  // ya resuelto; `startAt` es siempre el de la serie cuando viene.
  const std::string start = v["startAt"].is<const char*>() ? std::string(v["startAt"] | "")
                                                           : std::string(v["start"].is<const char*>() ? v["start"] | "" : "");
  date = v["date"] | "";
  if (date.empty() && start.size() >= 10) date = start.substr(0, 10);
  out.id = v["id"] | 0;
  out.kind = v["kind"] | "event";
  out.at = v["at"] | "";
  if (out.at.empty()) out.at = v["time"] | "";
  if (out.at.empty() && start.size() >= 16) out.at = start.substr(11, 5);
  out.title = v["title"] | "";
  out.place = v["place"] | "";
  out.note = v["note"] | "";
  out.endTime = v["endTime"] | "";
  out.startAt = start;
  out.endStamp = v["endAt"].is<const char*>() ? std::string(v["endAt"] | "") : std::string();
  JsonVariantConst rep = v["repeat"];
  // La repetición se guarda cruda: no se toca, solo se devuelve.
  if (!rep.isNull() && rep.is<JsonObjectConst>()) serializeJson(rep, out.repeatRaw);
}

// De la caché de la SD (los mismos campos, con los nombres cortos que se
// escriben ahí).
void readCachedItem(JsonVariantConst v, CalendarActivity::Item& out) {
  out.id = v["id"] | 0;
  out.kind = v["k"] | "event";
  out.at = v["at"] | "";
  out.title = v["title"] | "";
  out.place = v["place"] | "";
  out.note = v["note"] | "";
  out.endTime = v["et"] | "";
  out.startAt = v["s"] | "";
  out.endStamp = v["e"] | "";
  out.repeatRaw = v["rp"] | "";
}

void writeCachedItem(JsonObject o, const CalendarActivity::Item& it) {
  o["id"] = it.id;
  o["k"] = it.kind;
  o["at"] = it.at;
  o["title"] = it.title;
  if (!it.place.empty()) o["place"] = it.place;
  if (!it.note.empty()) o["note"] = it.note;
  if (!it.endTime.empty()) o["et"] = it.endTime;
  if (!it.startAt.empty()) o["s"] = it.startAt;
  if (!it.endStamp.empty()) o["e"] = it.endStamp;
  if (!it.repeatRaw.empty()) o["rp"] = it.repeatRaw;
}

std::string twoDigits(const int value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d", value);
  return buf;
}
}  // namespace

const char* CalendarActivity::monthName(const int month) {
  static const StrId ids[12] = {StrId::STR_CAL_MONTH_1, StrId::STR_CAL_MONTH_2,  StrId::STR_CAL_MONTH_3,
                                StrId::STR_CAL_MONTH_4, StrId::STR_CAL_MONTH_5,  StrId::STR_CAL_MONTH_6,
                                StrId::STR_CAL_MONTH_7, StrId::STR_CAL_MONTH_8,  StrId::STR_CAL_MONTH_9,
                                StrId::STR_CAL_MONTH_10, StrId::STR_CAL_MONTH_11, StrId::STR_CAL_MONTH_12};
  if (month < 1 || month > 12) return "";
  return I18N.get(ids[month - 1]);
}

const char* CalendarActivity::weekdayName(const int dow) {
  static const StrId ids[7] = {StrId::STR_CAL_DOW_0, StrId::STR_CAL_DOW_1, StrId::STR_CAL_DOW_2,
                               StrId::STR_CAL_DOW_3, StrId::STR_CAL_DOW_4, StrId::STR_CAL_DOW_5,
                               StrId::STR_CAL_DOW_6};
  if (dow < 0 || dow > 6) return "";
  return I18N.get(ids[dow]);
}

const char* CalendarActivity::weekdayShort(const int dow) {
  static const StrId ids[7] = {StrId::STR_CAL_DOW_S_0, StrId::STR_CAL_DOW_S_1, StrId::STR_CAL_DOW_S_2,
                               StrId::STR_CAL_DOW_S_3, StrId::STR_CAL_DOW_S_4, StrId::STR_CAL_DOW_S_5,
                               StrId::STR_CAL_DOW_S_6};
  if (dow < 0 || dow > 6) return "";
  return I18N.get(ids[dow]);
}

int CalendarActivity::daysInMonth(const int year, const int month) {
  static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return table[month - 1];
}

// days_from_civil / civil_from_days de Howard Hinnant: aritmética de fechas sin
// tocar la libc (mktime local no sirve acá, el aparato guarda el huso aparte).
long CalendarActivity::daysFromCivil(int year, const int month, const int day) {
  year -= month <= 2 ? 1 : 0;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const long yoe = year - era * 400;                                     // [0, 399]
  const long doy = (153L * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
  return era * 146097L + doe - 719468L;
}

void CalendarActivity::civilFromDays(const long days, int& year, int& month, int& day) {
  const long z = days + 719468L;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const long doe = z - era * 146097;                                          // [0, 146096]
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;      // [0, 399]
  const long y = yoe + era * 400;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                    // [0, 365]
  const long mp = (5 * doy + 2) / 153;                                         // [0, 11]
  const long d = doy - (153 * mp + 2) / 5 + 1;                                 // [1, 31]
  const long m = mp + (mp < 10 ? 3 : -9);                                      // [1, 12]
  year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  month = static_cast<int>(m);
  day = static_cast<int>(d);
}

int CalendarActivity::weekdayOfCivil(const int year, const int month, const int day) {
  // 1970-01-01 fue jueves: con lunes = 0, jueves es 3.
  const long d = daysFromCivil(year, month, day);
  return static_cast<int>(((d % 7) + 10) % 7);
}

namespace {
long localOffsetSeconds() {
  return (static_cast<long>(SETTINGS.clockUtcOffsetQ) - 48L) * 900L;
}
}  // namespace

time_t CalendarActivity::epochFromLocal(const int year, const int month, const int day, const int hour,
                                        const int minute) {
  const long days = daysFromCivil(year, month, day);
  return static_cast<time_t>(days * 86400L + hour * 3600L + minute * 60L - localOffsetSeconds());
}

void CalendarActivity::localFromEpoch(const time_t epoch, int& year, int& month, int& day, int& hour, int& minute) {
  long local = static_cast<long>(epoch) + localOffsetSeconds();
  long days = local / 86400L;
  long secs = local % 86400L;
  if (secs < 0) {  // la división de C trunca hacia cero: corrige antes de 1970
    secs += 86400L;
    days -= 1;
  }
  civilFromDays(days, year, month, day);
  hour = static_cast<int>(secs / 3600);
  minute = static_cast<int>((secs % 3600) / 60);
}

bool CalendarActivity::localToday(int& year, int& month, int& day) {
  time_t now = 0;
  if (!halClock.getEpochUtc(now) || now <= 0) return false;
  int hour = 0, minute = 0;
  localFromEpoch(now, year, month, day, hour, minute);
  return true;
}

std::string CalendarActivity::isoDate(const int year, const int month, const int day) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return buf;
}

void CalendarActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/.crosspoint");
  int y = 0, m = 0, d = 0;
  if (localToday(y, m, d)) {
    viewYear = y;
    viewMonth = m;
    cursorDay = d;
  }
  // Se entra al menú (Hoy / Calendario / Viajes) sin pedir nada: el mes se baja
  // recién cuando se abre el calendario, así entrar acá no prende el WiFi.
  loadMonthFromCache();
  state = HOME;
  homeRow = ROW_TODAY;
  requestUpdate();
}

// OK sobre el menú.
void CalendarActivity::openHomeRow() {
  switch (homeRow) {
    case ROW_TODAY:
      openToday();
      return;
    case ROW_TRIPS:
      startActivityForResult(std::make_unique<TripActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    default:
      break;
  }
  state = MONTH;
  requestUpdate();
  goToCurrentMonth();
}

// El mes en pantalla, bajado si la caché está vieja. Es lo que hacía onEnter
// antes de que el calendario tuviera menú.
void CalendarActivity::goToCurrentMonth() {
  const bool cached = monthCached;
  // Sin caché del mes (o con una vieja) se busca; si falla queda lo que haya.
  time_t now = 0;
  const bool haveClock = halClock.getEpochUtc(now);
  bool stale = !cached;
  if (cached && haveClock) {
    JsonDocument doc;
    if (readCache(doc)) {
      for (JsonVariantConst mv : doc["months"].as<JsonArrayConst>()) {
        if (std::string(mv["month"] | "") != monthKey(viewYear, viewMonth)) continue;
        const time_t savedAt = static_cast<time_t>(mv["savedAt"] | (int64_t)0);
        stale = savedAt == 0 || now - savedAt > CACHE_MAX_AGE_S;
      }
    }
  }
  if (stale) {
    afterLoad = MONTH;
    pending = MONTH_FETCH;
    ensureConnected();
  }
}

// ---------------------------------------------------------------------------
// Hoy: lo del día y las sugerencias
// ---------------------------------------------------------------------------

// Se entra a Hoy con lo que haya en la tarjeta y, si del día de hoy no hay
// nada guardado, se baja la agenda (eso sí es gratis). Las sugerencias NO se
// piden solas: cuestan plata, las pide el usuario con OK.
void CalendarActivity::openToday() {
  int y = 0, m = 0, d = 0;
  const bool haveClock = localToday(y, m, d);
  const std::string date = haveClock ? isoDate(y, m, d) : "";
  todayTop = 0;
  dayNotice.clear();
  menuItem = -1;
  state = TODAY;
  if (date.empty()) {
    dayItems.clear();
    buildTodayLines();
    requestUpdate();
    return;
  }
  const bool cached = loadDayFromCache(date);
  loadSuggestFromCache(date);
  buildTodayLines();
  requestUpdate();
  // Solo se prende el WiFi si de verdad falta algo: con el mes en la tarjeta y
  // hoy sin nada, no hay nada que bajar (si no, entrar a Hoy levantaría la red
  // todos los días de por vida).
  const DaySummary* s = summaryFor(d);
  if (!cached && (!monthCached || (s && s->count > 0))) {
    afterLoad = TODAY;
    pending = DAY_FETCH;
    ensureConnected();
  }
}

// El texto de la pantalla Hoy, ya armado: la fecha, lo que hay agendado y las
// sugerencias. Se rearma cada vez que cambia algo y después se pagina.
void CalendarActivity::buildTodayLines() {
  todayLines.clear();
  const int width = renderer.getScreenWidth() - 2 * SIDE - 8;
  auto push = [&](const std::string& text, const uint8_t style) { todayLines.push_back({text, style}); };
  auto pushWrapped = [&](const std::string& text, const uint8_t style) {
    const int font = style == 2 ? SMALL_FONT_ID : UI_10_FONT_ID;
    for (const std::string& part : renderer.wrappedText(font, text.c_str(), width, 8)) push(part, style);
  };

  int y = 0, m = 0, d = 0;
  if (localToday(y, m, d)) {
    push(std::string(weekdayName(weekdayOfCivil(y, m, d))) + " " + std::to_string(d) + " " + monthName(m), 1);
  } else {
    push(tr(STR_CAL_NO_CLOCK), 1);
  }

  // Lo que contestó el servidor al último dictado ("Cargué 3 actividades.").
  if (!dayNotice.empty()) pushWrapped(dayNotice, 0);

  push(tr(STR_DAY_AGENDA), 1);
  if (dayItems.empty()) {
    push(tr(STR_CAL_NO_EVENTS), 2);
  } else {
    for (const Item& it : dayItems) {
      std::string line = it.at.empty() ? it.title : it.at + "  " + it.title;
      if (!it.place.empty()) line += "  · " + it.place;
      pushWrapped(line, 0);
    }
  }

  push("", 0);
  push(tr(STR_SUGGEST_TITLE), 1);
  if (!suggestError.empty()) {
    pushWrapped(suggestError, 2);
  } else if (suggestLines.empty()) {
    pushWrapped(tr(STR_SUGGEST_EMPTY), 2);
  } else {
    for (const std::string& line : suggestLines) pushWrapped("• " + line, 0);
    // Cuándo se calcularon: son de la última vez que se pidieron, no de ahora.
    time_t now = 0;
    if (suggestAt > 0 && halClock.getEpochUtc(now) && now > suggestAt) {
      const long mins = static_cast<long>(now - suggestAt) / 60;
      char buf[64];
      if (mins < 60) snprintf(buf, sizeof(buf), tr(STR_SUGGEST_AGE_MIN), static_cast<int>(mins));
      else snprintf(buf, sizeof(buf), tr(STR_SUGGEST_AGE_HOUR), static_cast<int>(mins / 60));
      push(buf, 2);
    }
  }
}

// Las sugerencias del día viven en la misma caché del calendario, así que se
// ven sin WiFi y no se vuelven a pedir por entrar y salir.
bool CalendarActivity::loadSuggestFromCache(const std::string& date) {
  suggestLines.clear();
  suggestAt = 0;
  suggestDate = date;
  suggestError.clear();
  JsonDocument doc;
  if (!readCache(doc)) return false;
  JsonVariantConst sv = doc["suggest"];
  if (sv.isNull() || std::string(sv["date"] | "") != date) return false;
  suggestAt = static_cast<time_t>(sv["at"] | (int64_t)0);
  for (JsonVariantConst lv : sv["lines"].as<JsonArrayConst>()) {
    const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
    if (!line.empty()) suggestLines.push_back(line);
  }
  return !suggestLines.empty();
}

void CalendarActivity::saveSuggestToCache() const {
  JsonDocument doc;
  readCache(doc);
  JsonObject sv = doc["suggest"].to<JsonObject>();
  sv["date"] = suggestDate;
  sv["at"] = static_cast<int64_t>(suggestAt);
  JsonArray arr = sv["lines"].to<JsonArray>();
  for (const std::string& line : suggestLines) arr.add(line);
  writeCache(doc);
}

// GET /api/suggest/day. `refresh` solo cuando lo pidió el usuario con OK: cada
// recálculo le cuesta plata al dueño del servidor.
bool CalendarActivity::fetchSuggest(const bool refresh) {
  int y = 0, m = 0, d = 0;
  const std::string date = localToday(y, m, d) ? isoDate(y, m, d) : "";
  std::string path = "/api/suggest/day?lang=" + std::string(uiLanguageCode());
  if (!date.empty()) path += "&date=" + date;
  if (refresh) path += "&refresh=1";
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  suggestError.clear();
  if (r != ServerClient::Result::Ok) {
    // 429 = el servidor no quiere gastar más búsquedas hoy; se dice tal cual.
    suggestError = resp.status == 429 ? tr(STR_SUGGEST_BUDGET) : tr(STR_SUGGEST_FAILED);
    LOG_ERR(TAG, "GET /api/suggest/day: %s %d", ServerClient::resultName(r), resp.status);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    suggestError = tr(STR_SUGGEST_FAILED);
    return false;
  }
  suggestLines.clear();
  for (JsonVariantConst lv : doc["lines"].as<JsonArrayConst>()) {
    const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
    if (!line.empty()) suggestLines.push_back(line);
  }
  suggestAt = static_cast<time_t>(doc["at"] | (int64_t)0);
  suggestDate = date;
  if (suggestLines.empty()) suggestError = tr(STR_SUGGEST_EMPTY);
  else saveSuggestToCache();
  return !suggestLines.empty();
}

void CalendarActivity::onExit() {
  Activity::onExit();
  recorder.abort();  // el micrófono y la PSRAM de la toma no se quedan tomados
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void CalendarActivity::fail(const StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

const CalendarActivity::DaySummary* CalendarActivity::summaryFor(const int day) const {
  for (const DaySummary& s : summary) {
    if (s.day == day) return &s;
  }
  return nullptr;
}

// Un día para adelante o para atrás. Al pasarse del mes se cambia de mes solo:
// es la única forma de recorrer un año con dos direcciones sin perderse.
void CalendarActivity::moveDay(const int delta) {
  if (viewYear == 0) return;
  const long days = daysFromCivil(viewYear, viewMonth, cursorDay) + delta;
  int y = 0, m = 0, d = 0;
  civilFromDays(days, y, m, d);
  if (y != viewYear || m != viewMonth) {
    goToMonth(y, m, d);
  } else {
    cursorDay = d;
  }
  requestUpdate();
}

// Cambio de mes: primero la caché, y solo si el WiFi YA está arriba se pide al
// servidor. Levantar la red en cada salto dejaría la pantalla de selección
// apareciendo a mitad de un barrido con la palanca.
void CalendarActivity::goToMonth(const int year, const int month, const int day) {
  viewYear = year;
  viewMonth = month;
  cursorDay = std::min(std::max(day, 1), daysInMonth(year, month));
  summary.clear();
  monthCached = loadMonthFromCache();
  if (!monthCached && ServerClient::networkUp()) {
    pending = MONTH_FETCH;
    state = LOADING;
  }
}

bool CalendarActivity::loadMonthFromCache() {
  summary.clear();
  if (viewYear == 0) return false;
  JsonDocument doc;
  if (!readCache(doc)) return false;
  const std::string key = monthKey(viewYear, viewMonth);
  for (JsonVariantConst mv : doc["months"].as<JsonArrayConst>()) {
    if (std::string(mv["month"] | "") != key) continue;
    for (JsonVariantConst dv : mv["days"].as<JsonArrayConst>()) {
      DaySummary s;
      s.day = dv["d"] | 0;
      s.count = dv["c"] | 0;
      s.firstTitle = dv["t"] | "";
      if (s.day >= 1 && s.day <= 31) summary.push_back(std::move(s));
    }
    monthCached = true;
    return true;
  }
  return false;
}

bool CalendarActivity::loadDayFromCache(const std::string& date) {
  dayItems.clear();
  dayDate = date;
  JsonDocument doc;
  if (!readCache(doc)) return false;
  for (JsonVariantConst dv : doc["days"].as<JsonArrayConst>()) {
    if (std::string(dv["date"] | "") != date) continue;
    for (JsonVariantConst iv : dv["items"].as<JsonArrayConst>()) {
      Item it;
      readCachedItem(iv, it);
      dayItems.push_back(std::move(it));
    }
    return true;
  }
  return false;
}

// GET /api/calendar: el resumen por día pinta el mes, y si el servidor manda
// además los eventos, se guardan por día para poder mirarlos sin WiFi.
bool CalendarActivity::fetchMonth() {
  std::string path = "/api/calendar?lang=" + std::string(uiLanguageCode());
  if (viewYear != 0) {
    path += "&from=" + isoDate(viewYear, viewMonth, 1);
    path += "&to=" + isoDate(viewYear, viewMonth, daysInMonth(viewYear, viewMonth));
  }
  ServerClient::Response resp;
  ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok) {
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/calendar: %s", failureDetail.c_str());
    return false;
  }
  // Un mes con muchísimos eventos no entra en el heap una vez leído y otra vez
  // parseado, así que se vuelve a pedir solo el resumen (`summary=1`): la
  // cuadrícula se pinta igual y el día se baja cuando se entra.
  if (resp.body.size() > MAX_MONTH_BODY) {
    LOG_INF(TAG, "mes muy grande (%u bytes): solo el resumen", (unsigned)resp.body.size());
    r = SERVER_CLIENT.get(path + "&summary=1", resp);
    if (r != ServerClient::Result::Ok) return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;

  summary.clear();
  for (JsonVariantConst dv : doc["days"].as<JsonArrayConst>()) {
    const std::string date = dv["date"] | "";
    DaySummary s;
    s.day = dayOfIso(date);
    s.count = dv["count"] | 0;
    s.firstTitle = dv["firstTitle"] | "";
    if (s.day < 1) continue;
    // Sin reloj ni caché el mes se saca de la respuesta: el servidor devuelve
    // el mes en curso cuando no se le manda rango.
    if (viewYear == 0 && date.size() >= 10) {
      viewYear = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
      viewMonth = (date[5] - '0') * 10 + (date[6] - '0');
      cursorDay = 1;
    }
    summary.push_back(std::move(s));
  }
  if (viewYear == 0) return false;

  // A la caché: el mes, y los días que el servidor haya mandado enteros.
  JsonDocument cache;
  readCache(cache);
  JsonArray months = cache["months"].isNull() ? cache["months"].to<JsonArray>() : cache["months"].as<JsonArray>();
  const std::string key = monthKey(viewYear, viewMonth);
  for (int i = static_cast<int>(months.size()) - 1; i >= 0; --i) {
    if (std::string(months[i]["month"] | "") == key) months.remove(i);
  }
  time_t now = 0;
  halClock.getEpochUtc(now);
  {
    JsonObject mo = months.add<JsonObject>();
    mo["month"] = key;
    mo["savedAt"] = static_cast<int64_t>(now);
    JsonArray da = mo["days"].to<JsonArray>();
    for (const DaySummary& s : summary) {
      JsonObject o = da.add<JsonObject>();
      o["d"] = s.day;
      o["c"] = s.count;
      o["t"] = s.firstTitle;
    }
  }
  while (static_cast<int>(months.size()) > MAX_CACHED_MONTHS) months.remove(0);

  JsonArrayConst events = doc["events"].as<JsonArrayConst>();
  if (!events.isNull()) {
    JsonArray days = cache["days"].isNull() ? cache["days"].to<JsonArray>() : cache["days"].as<JsonArray>();
    // Todos los días de este mes se rearman desde cero con lo que llegó.
    for (int i = static_cast<int>(days.size()) - 1; i >= 0; --i) {
      const std::string d = days[i]["date"] | "";
      if (d.size() >= 7 && d.compare(0, 7, key) == 0) days.remove(i);
    }
    for (JsonVariantConst ev : events) {
      std::string date;
      Item it;
      readEvent(ev, it, date);
      if (date.size() < 10 || it.title.empty()) continue;
      JsonObject target;
      for (JsonVariant dv : days) {
        if (std::string(dv["date"] | "") == date) {
          target = dv.as<JsonObject>();
          break;
        }
      }
      if (target.isNull()) {
        target = days.add<JsonObject>();
        target["date"] = date;
        target["savedAt"] = static_cast<int64_t>(now);
        target["items"].to<JsonArray>();
      }
      writeCachedItem(target["items"].as<JsonArray>().add<JsonObject>(), it);
    }
    while (static_cast<int>(days.size()) > MAX_CACHED_DAYS) days.remove(0);
  }
  writeCache(cache);
  monthCached = true;
  return true;
}

bool CalendarActivity::fetchDay(const std::string& date) {
  ServerClient::Response resp;
  const std::string path = "/api/calendar/day?date=" + date + "&lang=" + std::string(uiLanguageCode());
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok) {
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/calendar/day: %s", failureDetail.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (items.isNull()) items = doc["events"].as<JsonArrayConst>();
  dayItems.clear();
  dayDate = date;
  for (JsonVariantConst iv : items) {
    std::string d;
    Item it;
    readEvent(iv, it, d);
    if (it.title.empty()) continue;
    dayItems.push_back(std::move(it));
  }
  saveDayToCache();
  syncSummaryCount();
  return true;
}

// Lo que hay en `dayItems` para `dayDate`, a la caché de la SD. Se llama
// después de bajar el día y después de CADA cambio hecho en el aparato, así lo
// que se ve sigue estando ahí sin WiFi y sin esperar la próxima bajada.
void CalendarActivity::saveDayToCache() const {
  if (dayDate.size() < 10) return;
  JsonDocument cache;
  readCache(cache);
  JsonArray days = cache["days"].isNull() ? cache["days"].to<JsonArray>() : cache["days"].as<JsonArray>();
  for (int i = static_cast<int>(days.size()) - 1; i >= 0; --i) {
    if (std::string(days[i]["date"] | "") == dayDate) days.remove(i);
  }
  time_t now = 0;
  halClock.getEpochUtc(now);
  JsonObject do_ = days.add<JsonObject>();
  do_["date"] = dayDate;
  do_["savedAt"] = static_cast<int64_t>(now);
  JsonArray arr = do_["items"].to<JsonArray>();
  for (const Item& it : dayItems) writeCachedItem(arr.add<JsonObject>(), it);
  while (static_cast<int>(days.size()) > MAX_CACHED_DAYS) days.remove(0);
  writeCache(cache);
}

// El punto y el número de la cuadrícula salen del resumen del mes: después de
// cargar o borrar actividades hay que moverlo a mano o el mes queda mintiendo
// hasta la próxima bajada.
void CalendarActivity::syncSummaryCount() {
  if (dayDate.size() < 10) return;
  const int day = dayOfIso(dayDate);
  if (day < 1) return;
  if (dayDate.compare(0, 7, monthKey(viewYear, viewMonth)) != 0) return;
  const int count = static_cast<int>(dayItems.size());
  for (DaySummary& sum : summary) {
    if (sum.day != day) continue;
    sum.count = count;
    sum.firstTitle = count ? dayItems[0].title : std::string();
    return;
  }
  if (count == 0) return;
  summary.push_back({day, count, dayItems[0].title});
}

// El mismo orden que manda el servidor: primero lo de todo el día y después
// por hora. Se usa cuando se le cambia la hora a una actividad acá.
void CalendarActivity::sortDayItems() {
  std::stable_sort(dayItems.begin(), dayItems.end(), [](const Item& a, const Item& b) {
    if (a.at.empty() != b.at.empty()) return a.at.empty();
    return a.at < b.at;
  });
}

// OK sobre un día: lo que ya está cacheado se abre en el acto, y solo se pide
// al servidor cuando el resumen dice que hay algo y no lo tenemos.
void CalendarActivity::openDay() {
  if (viewYear == 0) return;
  const std::string date = isoDate(viewYear, viewMonth, cursorDay);
  const DaySummary* s = summaryFor(cursorDay);
  const bool cached = loadDayFromCache(date);
  dayIndex = 0;
  dayNotice.clear();
  menuItem = -1;
  if (cached || !s || s->count == 0) {
    state = DAY;
    requestUpdate();
    return;
  }
  afterLoad = DAY;
  pending = DAY_FETCH;
  ensureConnected();
}


// ---------------------------------------------------------------------------
// Dictar el día, y cambiar o borrar una actividad
// ---------------------------------------------------------------------------

// El aparato no tiene teclado: TODO lo que el usuario escribe entra por el
// micrófono. Acá se graba de una vez el día entero ("a las 8 gimnasio, a las 9
// reunión con Ana") y el servidor lo parte en actividades; el mismo camino, con
// REC_TITLE, sirve para volver a decir el título de una que ya está.
void CalendarActivity::startDictation(const RecordMode mode) {
  recordMode = mode;
  // Se dicta desde la vista del día y también desde Hoy (Atrás mantenido): se
  // vuelve a la pantalla desde la que se arrancó.
  if (state == TODAY || state == DAY) dictateReturn = state;
  dayNotice.clear();
  transcribed.clear();
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    dayNotice = I18N.get(why);
    state = dictateReturn;
    if (state == TODAY) buildTodayLines();
    requestUpdate();
    return;
  }
  state = DICTATING;
  requestUpdate();
}

void CalendarActivity::stopDictation() {
  recorder.stop();
  if (recorder.tooShort()) {  // apretón sin querer
    recorder.abort();
    state = dictateReturn;
    requestUpdate();
    return;
  }
  afterLoad = DAY;
  afterWifi = recordMode == REC_DAY ? DICTATE_SEND : TITLE_SEND;
  pending = afterWifi;
  ensureConnected();
}

// POST /api/transcribe y después POST /api/calendar/dictate. Los dos necesitan
// respuesta, así que van con el WiFi arriba; la cola offline no sirve acá.
void CalendarActivity::performDictate() {
  std::string detail;
  const bool heard = SpeechToText::transcribe(recorder, transcribed, detail);
  recorder.release();
  if (!heard) {
    dayNotice = detail.empty() ? tr(STR_ASK_TRANSCRIBE_FAILED) : detail;
    return;
  }
  // Sin reloj ni día abierto no se manda fecha: el servidor usa su hoy, que es
  // mejor que mandarle un "0000-00-00".
  const std::string date = dayDate.size() >= 10 ? dayDate
                           : viewYear > 0      ? isoDate(viewYear, viewMonth, cursorDay)
                                               : std::string();
  std::string body;
  {
    JsonDocument doc;
    doc["text"] = transcribed;
    if (date.size() >= 10) doc["date"] = date;
    doc["lang"] = uiLanguageCode();
    serializeJson(doc, body);
  }
  ServerClient::Response resp;
  const ServerClient::Result r =
      SERVER_CLIENT.postJson("/api/calendar/dictate", body, resp, DICTATE_TIMEOUT_MS);
  if (r != ServerClient::Result::Ok) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%d)", ServerClient::resultName(r), resp.status);
    dayNotice = std::string(tr(STR_CAL_DICTATE_FAILED)) + " · " + buf;
    LOG_ERR(TAG, "POST /api/calendar/dictate: %s", buf);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    dayNotice = tr(STR_CAL_DICTATE_FAILED);
    return;
  }
  // `reply` es la frase corta que arma el servidor ("Cargué 3 actividades.").
  dayNotice = doc["reply"] | "";
  if (dayNotice.empty()) dayNotice = doc["error"] | tr(STR_CAL_DICTATE_FAILED);
  LOG_INF(TAG, "dictado: %s", dayNotice.c_str());
  // Y el día se vuelve a bajar: así las actividades nuevas aparecen con su id,
  // que es lo que hace falta para poder cambiarlas o borrarlas.
  if (date.size() >= 10) fetchDay(date);
  dayIndex = 0;
}

// El título nuevo de una actividad, dicho por voz. La transcripción necesita
// WiFi; el POST que la guarda ya puede esperar en la cola.
void CalendarActivity::performTitle() {
  std::string detail;
  const bool heard = SpeechToText::transcribe(recorder, transcribed, detail);
  recorder.release();
  if (!heard) {
    dayNotice = detail.empty() ? tr(STR_ASK_TRANSCRIBE_FAILED) : detail;
    return;
  }
  if (menuItem < 0 || menuItem >= static_cast<int>(dayItems.size())) return;
  dayItems[menuItem].title = transcribed;
  saveDayToCache();
  syncSummaryCount();
  sendEventEdit(dayItems[menuItem]);
  dayNotice = transcribed;
}

// POST /api/calendar/event con el id: edita en su lugar. Va por la cola, así
// el cambio queda hecho aunque no haya WiFi. OJO: el servidor REEMPLAZA el
// evento con lo que le llega, por eso se le devuelve todo lo que vino de él
// (lugar, nota, fin y repetición), no solo lo que se cambió.
void CalendarActivity::sendEventEdit(const Item& it) const {
  if (it.id <= 0) return;
  const std::string date = it.startAt.size() >= 10 ? it.startAt.substr(0, 10) : dayDate;
  if (date.size() < 10) return;
  std::string body;
  {
    JsonDocument doc;
    doc["id"] = it.id;
    doc["title"] = it.title;
    doc["date"] = date;
    doc["time"] = it.at;  // vacío = actividad de todo el día
    doc["endDate"] = it.endStamp.size() >= 10 ? it.endStamp.substr(0, 10) : date;
    doc["endTime"] = it.endTime;
    doc["place"] = it.place;
    doc["note"] = it.note;
    if (!it.repeatRaw.empty()) {
      JsonDocument rep;
      if (deserializeJson(rep, it.repeatRaw) == DeserializationError::Ok) doc["repeat"] = rep.as<JsonVariantConst>();
    }
    serializeJson(doc, body);
  }
  const ServerClient::Result r =
      SERVER_CLIENT.postOrQueue("/api/calendar/event?lang=" + std::string(uiLanguageCode()), body);
  LOG_INF(TAG, "event %d: %s", it.id, ServerClient::resultName(r));
}

void CalendarActivity::deleteMenuItem() {
  if (menuItem < 0 || menuItem >= static_cast<int>(dayItems.size())) return;
  const int id = dayItems[menuItem].id;
  dayItems.erase(dayItems.begin() + menuItem);
  if (dayIndex >= static_cast<int>(dayItems.size()) && dayIndex > 0) dayIndex--;
  saveDayToCache();
  syncSummaryCount();
  if (id > 0) {
    std::string body;
    {
      JsonDocument doc;
      doc["id"] = id;
      serializeJson(doc, body);
    }
    const ServerClient::Result r = SERVER_CLIENT.postOrQueue("/api/calendar/event/delete", body);
    LOG_INF(TAG, "delete event %d: %s", id, ServerClient::resultName(r));
  }
  menuItem = -1;
}

// Atrás mantenido sobre una actividad: el mismo menú por ítem que ya tienen las
// listas de AgendaActivity.
void CalendarActivity::openItemMenu() {
  if (state != DAY || dayItems.empty()) return;
  if (dayIndex < 0 || dayIndex >= static_cast<int>(dayItems.size())) return;
  // Los recordatorios viven en el store (se tildan en Recordatorios) y los
  // ítems de viaje se cargan en la web: acá solo se tocan las actividades.
  if (dayItems[dayIndex].kind != "event" || dayItems[dayIndex].id <= 0) {
    dayNotice = tr(STR_CAL_ONLY_EVENTS);
    requestUpdate();
    return;
  }
  menuItem = dayIndex;
  menuOptions = {tr(STR_CAL_ITEM_TIME), tr(STR_CAL_ITEM_TITLE), tr(STR_CAL_ITEM_DELETE)};
  menuOpen = true;
  menu.show(StrId::STR_CAL_ITEM_MENU, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
  requestUpdate();
}

void CalendarActivity::onMenuPick(const int index) {
  menuOpen = false;
  if (index == 0) {
    openTimeEditor();
    return;
  }
  if (index == 1) {
    startDictation(REC_TITLE);
    return;
  }
  if (index == 2) deleteMenuItem();
  requestUpdate();
}

// La hora se ELIGE con la palanca (el aparato no tiene teclado ni para los
// números): horas primero, después minutos de cinco en cinco.
void CalendarActivity::openTimeEditor() {
  if (menuItem < 0 || menuItem >= static_cast<int>(dayItems.size())) return;
  const std::string& at = dayItems[menuItem].at;
  if (at.size() >= 5) {
    editHour = (at[0] - '0') * 10 + (at[1] - '0');
    editMinute = (at[3] - '0') * 10 + (at[4] - '0');
  } else {
    editHour = -1;
    editMinute = 0;
  }
  if (editHour < -1 || editHour > 23) editHour = -1;
  editMinute = (editMinute / 5) * 5;
  editMinuteField = false;
  state = TIME_EDIT;
  requestUpdate();
}

void CalendarActivity::timeStep(const int delta) {
  if (editMinuteField) {
    editMinute = ((editMinute / 5 + delta) % 12 + 12) % 12 * 5;
  } else {
    // -1 ("sin hora") entra en la rueda justo antes de las 00: así se le puede
    // sacar la hora a una actividad sin otro menú.
    editHour = ((editHour + 1 + delta) % 25 + 25) % 25 - 1;
  }
  requestUpdate();
}

void CalendarActivity::confirmTimeEditor() {
  if (!editMinuteField && editHour >= 0) {
    editMinuteField = true;
    requestUpdate();
    return;
  }
  if (menuItem >= 0 && menuItem < static_cast<int>(dayItems.size())) {
    Item& it = dayItems[menuItem];
    it.at = editHour < 0 ? std::string() : twoDigits(editHour) + ":" + twoDigits(editMinute);
    // El fin que quedó antes del arranque lo recorta el servidor; acá se saca
    // para que lo que se ve en pantalla no mienta.
    if (!it.at.empty() && !it.endTime.empty() && it.endTime < it.at) it.endTime.clear();
    const Item copy = it;
    sortDayItems();
    saveDayToCache();
    syncSummaryCount();
    sendEventEdit(copy);
    // El cursor sigue a la actividad que se movió de lugar en la lista.
    for (int i = 0; i < static_cast<int>(dayItems.size()); ++i) {
      if (dayItems[i].id == copy.id) {
        dayIndex = i;
        break;
      }
    }
  }
  menuItem = -1;
  state = DAY;
  requestUpdate();
}

void CalendarActivity::ensureConnected() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void CalendarActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    // Sin red se sigue mirando lo que haya en la tarjeta: el calendario tiene
    // que servir sin WiFi.
    wifiActivated = false;
    const Pending was = pending;
    pending = NONE;
    // Dictar y cambiar el título necesitan que el servidor conteste: sin WiFi
    // no se puede, y se dice en el día en vez de tirar al usuario al mes.
    if (was == DICTATE_SEND || was == TITLE_SEND) {
      recorder.abort();
      afterWifi = NONE;
      dayNotice = tr(STR_SERVER_WIFI_FAILED);
      state = dictateReturn;
      if (state == TODAY) buildTodayLines();
      requestUpdate();
      return;
    }
    state = afterLoad == TODAY ? TODAY : MONTH;
    if (state == TODAY) {
      suggestError = tr(STR_SERVER_WIFI_FAILED);
      buildTodayLines();
    }
    requestUpdate();
    return;
  }
  state = LOADING;
  requestUpdate();
}

void CalendarActivity::loop() {
  // El menú de la actividad se come toda la entrada mientras está abierto.
  if (menuOpen) {
    if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!menu.isActive() && menuOpen) {  // Atrás sobre el menú: se cierra y punto
        menuOpen = false;
        menuItem = -1;
        requestUpdate();
      }
    }
    return;
  }

  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      const Pending p = pending;
      pending = NONE;
      if (p == MONTH_FETCH) {
        const bool ok = fetchMonth();
        WiFi.setSleep(true);
        state = MONTH;
        if (!ok) LOG_ERR(TAG, "mes: %s", failureDetail.c_str());
        requestUpdate();
      } else if (p == DAY_FETCH) {
        const bool ok = fetchDay(dayDate.empty() ? isoDate(viewYear, viewMonth, cursorDay) : dayDate);
        WiFi.setSleep(true);
        state = afterLoad == TODAY ? TODAY : DAY;
        dayIndex = 0;
        if (state == TODAY) buildTodayLines();
        if (!ok) LOG_ERR(TAG, "día: %s", failureDetail.c_str());
        requestUpdate();
      } else if (p == DICTATE_SEND || p == TITLE_SEND) {
        if (p == DICTATE_SEND) performDictate();
        else performTitle();
        WiFi.setSleep(true);
        afterWifi = NONE;
        menuItem = -1;
        state = dictateReturn;
        if (state == TODAY) {
          todayTop = 0;
          buildTodayLines();
        }
        requestUpdate();
      } else if (p == SUGGEST_FETCH) {
        fetchSuggest(suggestRefresh);
        suggestRefresh = false;
        WiFi.setSleep(true);
        state = TODAY;
        todayTop = 0;
        buildTodayLines();
        requestUpdate();
      } else {
        state = MONTH;
        requestUpdate();
      }
      break;
    }
    case HOME: {
      buttonNavigator.onNext([this] {
        homeRow = ButtonNavigator::nextIndex(homeRow, HOME_ROWS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        homeRow = ButtonNavigator::previousIndex(homeRow, HOME_ROWS);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openHomeRow();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      break;
    }
    case TODAY: {
      // Atrás mantenido dicta el día de hoy: OK ya está tomado por las
      // sugerencias, y este es el lugar donde uno mira la jornada.
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
        startDictation(REC_DAY);
        break;
      }
      const int total = static_cast<int>(todayLines.size());
      buttonNavigator.onNext([&] {
        if (todayTop + todayPerPage < total) todayTop += todayPerPage;
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        todayTop = std::max(0, todayTop - todayPerPage);
        requestUpdate();
      });
      // OK pide (o vuelve a pedir) las sugerencias: es la ÚNICA forma de que el
      // servidor gaste una búsqueda, así no se van los pesos solos.
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        suggestRefresh = !suggestLines.empty();
        afterLoad = TODAY;
        pending = SUGGEST_FETCH;
        ensureConnected();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = HOME;
        requestUpdate();
      }
      break;
    }
    case MONTH: {
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, REFRESH_HOLD_MS)) {
        afterLoad = MONTH;
        pending = MONTH_FETCH;
        ensureConnected();
        break;
      }
      buttonNavigator.onNext([this] { moveDay(1); });
      buttonNavigator.onPrevious([this] { moveDay(-1); });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openDay();
        break;
      }
      // Atrás vuelve al menú de Mi día, no al hub: siempre se sale o se vuelve.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = HOME;
        requestUpdate();
      }
      break;
    }
    case DAY: {
      // Atrás mantenido: el menú de la actividad marcada (hora, título, borrar).
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
        openItemMenu();
        break;
      }
      const int count = static_cast<int>(dayItems.size());
      buttonNavigator.onNext([&] {
        if (count > 0) dayIndex = ButtonNavigator::nextIndex(dayIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) dayIndex = ButtonNavigator::previousIndex(dayIndex, count);
        requestUpdate();
      });
      // OK dicta el día entero: es la forma de cargar actividades sin teclado.
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startDictation(REC_DAY);
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        dayNotice.clear();
        state = MONTH;
        requestUpdate();
      }
      break;
    }
    case DICTATING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        state = dictateReturn;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        stopDictation();
        break;
      }
      if (!recorder.pump()) {
        recorder.abort();
        dayNotice = tr(STR_AUDIO_CAPTURE_FAILED);
        state = dictateReturn;
        if (state == TODAY) buildTodayLines();
        requestUpdate();
      }
      break;
    case TIME_EDIT:
      buttonNavigator.onNext([this] { timeStep(1); });
      buttonNavigator.onPrevious([this] { timeStep(-1); });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        confirmTimeEditor();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        // Atrás siempre sale: primero de los minutos, después sin guardar nada.
        if (editMinuteField) {
          editMinuteField = false;
        } else {
          menuItem = -1;
          state = DAY;
        }
        requestUpdate();
      }
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = HOME;
        requestUpdate();
      }
      break;
    case CONNECTING:
      break;
  }
}

// El menú de Mi día: tres filas grandes con lo que hay adentro.
void CalendarActivity::renderHome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + 16;
  const int rowH = 76;
  const StrId titles[HOME_ROWS] = {StrId::STR_DAY_TODAY, StrId::STR_CAL_TITLE, StrId::STR_DAY_TRIPS};
  const StrId subs[HOME_ROWS] = {StrId::STR_DAY_TODAY_SUB, StrId::STR_DAY_CALENDAR_SUB, StrId::STR_DAY_TRIPS_SUB};
  for (int i = 0; i < HOME_ROWS; ++i) {
    const int y = top + i * (rowH + 10);
    const bool sel = i == homeRow;
    if (sel) drawSelectionRow(renderer, SIDE, y, pageWidth - 2 * SIDE, rowH, 12);
    else renderer.drawRoundedRect(SIDE, y, pageWidth - 2 * SIDE, rowH, 2, 12, true);
    const int tw = pageWidth - 2 * SIDE - 32;
    renderer.drawText(UI_12_FONT_ID, SIDE + 16, y + 14,
                      renderer.truncatedText(UI_12_FONT_ID, I18N.get(titles[i]), tw, EpdFontFamily::BOLD).c_str(), SELECTION_INK,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, SIDE + 16, y + 44,
                      renderer.truncatedText(SMALL_FONT_ID, I18N.get(subs[i]), tw).c_str(), SELECTION_INK);
  }
}

// Hoy: la agenda del día y las sugerencias, paginadas con la palanca.
void CalendarActivity::renderToday() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + 12;
  // El margen de abajo lleva verticalSpacing además del alto de los hints.
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 18;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  todayPerPage = std::max(1, (bottom - top) / lineH);
  if (todayLines.empty()) buildTodayLines();
  const int total = static_cast<int>(todayLines.size());
  if (todayTop >= total) todayTop = 0;
  for (int i = 0; i < todayPerPage && todayTop + i < total; ++i) {
    const Line& line = todayLines[todayTop + i];
    if (line.text.empty()) continue;
    const int font = line.style == 2 ? SMALL_FONT_ID : line.style == 1 ? UI_12_FONT_ID : UI_10_FONT_ID;
    const auto style = line.style == 1 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(font, SIDE, top + i * lineH,
                      renderer.truncatedText(font, line.text.c_str(), pageWidth - 2 * SIDE, style).c_str(), true,
                      style);
  }
  if (total > todayPerPage) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d/%d", todayTop / todayPerPage + 1, (total + todayPerPage - 1) / todayPerPage);
    renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom, pages);
  }
  // El dictado está en Atrás mantenido y sin decirlo no lo encuentra nadie.
  renderer.drawText(SMALL_FONT_ID, SIDE, bottom,
                    renderer.truncatedText(SMALL_FONT_ID, tr(STR_DAY_DICTATE_HINT), pageWidth - 2 * SIDE - 60).c_str());
}

void CalendarActivity::renderMonth() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (viewYear == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_CAL_NO_CLOCK));
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, tr(STR_CAL_REFRESH_HINT));
    return;
  }

  const int cols = 7;
  const int cellW = (pageWidth - 2 * SIDE) / cols;
  const int gridX = (pageWidth - cellW * cols) / 2;
  const int dowY = metrics.topPadding + metrics.headerHeight + 8;
  const int gridTop = dowY + renderer.getLineHeight(SMALL_FONT_ID) + 6;
  // Abajo van dos líneas: qué hay en el día marcado y la ayuda de los botones.
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  // Abajo de la cuadrícula entran dos líneas (qué cae en el día marcado) y la
  // ayuda de los botones: si no se les reserva lugar, la última fila de la
  // cuadrícula se les encima.
  const int infoH = 72;
  const int cellH = std::max(34, (bottom - infoH - gridTop) / 6);

  for (int c = 0; c < cols; ++c) {
    const char* label = weekdayShort(c);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, gridX + c * cellW + (cellW - w) / 2, dowY, label);
  }

  const int firstDow = weekdayOfCivil(viewYear, viewMonth, 1);
  const int dim = daysInMonth(viewYear, viewMonth);
  int todayY = 0, todayM = 0, todayD = 0;
  const bool haveToday = localToday(todayY, todayM, todayD);

  for (int cell = 0; cell < 42; ++cell) {
    const int day = cell - firstDow + 1;
    if (day < 1 || day > dim) continue;
    const int x = gridX + (cell % cols) * cellW;
    const int y = gridTop + (cell / cols) * cellH;
    const bool selected = day == cursorDay;
    const bool isToday = haveToday && todayY == viewYear && todayM == viewMonth && todayD == day;
    if (selected) drawSelectionRow(renderer, x + 2, y + 2, cellW - 4, cellH - 6);
    else if (isToday) renderer.drawRoundedRect(x + 2, y + 2, cellW - 4, cellH - 6, 2, 8, true);

    char num[4];
    snprintf(num, sizeof(num), "%d", day);
    const int nw = renderer.getTextWidth(UI_12_FONT_ID, num);
    renderer.drawText(UI_12_FONT_ID, x + (cellW - nw) / 2, y + 8, num, SELECTION_INK);

    const DaySummary* s = summaryFor(day);
    if (s && s->count > 0) {
      const int dotY = y + cellH - 22;
      if (s->count == 1) {
        renderer.fillRoundedRect(x + cellW / 2 - 4, dotY, 8, 8, 4, Color::Black);
      } else {
        char n[8];
        snprintf(n, sizeof(n), "%d", s->count);
        const int w = renderer.getTextWidth(SMALL_FONT_ID, n);
        renderer.fillRoundedRect(x + cellW / 2 - w / 2 - 8, dotY - 1, 6, 6, 3, Color::Black);
        renderer.drawText(SMALL_FONT_ID, x + cellW / 2 - w / 2 + 2, dotY - 6, n, SELECTION_INK);
      }
    }
  }

  // Qué cae en el día marcado, en palabras: es lo que evita tener que entrar
  // para saber si el cursor quedó donde uno cree.
  const int infoY = gridTop + 6 * cellH + 4;
  std::string line = std::string(weekdayName(weekdayOfCivil(viewYear, viewMonth, cursorDay))) + " " +
                     std::to_string(cursorDay);
  int ty = 0, tm = 0, td = 0;
  if (localToday(ty, tm, td) && ty == viewYear && tm == viewMonth && td == cursorDay) {
    line += " · " + std::string(tr(STR_CAL_TODAY));
  }
  renderer.drawText(UI_10_FONT_ID, SIDE + 4, infoY,
                    renderer.truncatedText(UI_10_FONT_ID, line.c_str(), pageWidth - 2 * SIDE - 8).c_str());
  const DaySummary* s = summaryFor(cursorDay);
  std::string detail = tr(STR_CAL_NO_EVENTS);
  if (s && s->count > 0) {
    detail = s->firstTitle;
    if (s->count > 1) {
      char n[32];
      snprintf(n, sizeof(n), tr(STR_CAL_EVENTS_FORMAT), s->count);
      detail = std::string(n) + (detail.empty() ? "" : " · " + detail);
    }
  }
  renderer.drawText(SMALL_FONT_ID, SIDE + 4, infoY + 22,
                    renderer.truncatedText(SMALL_FONT_ID, detail.c_str(), pageWidth - 2 * SIDE - 8).c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, bottom - 18,
                            renderer.truncatedText(SMALL_FONT_ID, tr(STR_CAL_HINT), pageWidth - 2 * SIDE).c_str());
}

void CalendarActivity::renderDay() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int top = metrics.topPadding + metrics.headerHeight + 12;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  // Lo que contestó el servidor al dictado ("Cargué 3 actividades."), o por qué
  // no se pudo. Se va solo en cuanto se sale del día.
  if (!dayNotice.empty()) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, dayNotice.c_str(), pageWidth - 2 * SIDE, 2);
    for (size_t i = 0; i < lines.size(); ++i) {
      renderer.drawText(UI_10_FONT_ID, SIDE, top + static_cast<int>(i) * 20, lines[i].c_str(), true,
                        EpdFontFamily::BOLD);
    }
    top += static_cast<int>(lines.size()) * 20 + 8;
  }
  // Abajo de las filas van el "p/N" y la línea que dice qué hacen los botones.
  const int hintH = 40;
  itemsPerPage = std::max(1, (bottom - hintH - top) / ROW_H);
  const int count = static_cast<int>(dayItems.size());
  renderer.drawCenteredText(SMALL_FONT_ID, bottom - 18,
                            renderer.truncatedText(SMALL_FONT_ID, tr(STR_CAL_ITEM_HINT), pageWidth - 2 * SIDE).c_str());
  if (count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, (top + bottom) / 2 - 10, tr(STR_CAL_NO_EVENTS));
    return;
  }
  const int first = (dayIndex / itemsPerPage) * itemsPerPage;
  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * ROW_H;
    const bool sel = i == dayIndex;
    if (sel) drawSelectionRow(renderer, SIDE + 2, y, pageWidth - 2 * (SIDE + 2), ROW_H - 6);
    const Item& it = dayItems[i];
    const int atW = it.at.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, it.at.c_str()) + 12;
    if (atW) renderer.drawText(SMALL_FONT_ID, SIDE + 10, y + 10, it.at.c_str(), SELECTION_INK);
    renderer.drawText(UI_12_FONT_ID, SIDE + 10 + atW, y + 6,
                      renderer.truncatedText(UI_12_FONT_ID, it.title.c_str(), pageWidth - 2 * SIDE - 20 - atW).c_str(),
                      SELECTION_INK);
    if (!it.place.empty()) {
      renderer.drawText(SMALL_FONT_ID, SIDE + 10 + atW, y + 30,
                        renderer.truncatedText(SMALL_FONT_ID, it.place.c_str(), pageWidth - 2 * SIDE - 20 - atW).c_str(),
                        SELECTION_INK);
    }
  }
  if (count > itemsPerPage) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d/%d", dayIndex / itemsPerPage + 1, (count + itemsPerPage - 1) / itemsPerPage);
    renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom - 38,
                      pages);
  }
}

// Grabando: qué se puede decir, para que no haya que adivinar el formato.
void CalendarActivity::renderDictating() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + 30;
  const bool title = recordMode == REC_TITLE;
  renderer.drawCenteredText(
      UI_12_FONT_ID, top,
      renderer.truncatedText(UI_12_FONT_ID, title ? tr(STR_CAL_TITLE_PROMPT) : tr(STR_CAL_DICTATE_PROMPT),
                             pageWidth - 30, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);
  const char* hint = title ? tr(STR_CAL_TITLE_HINT) : tr(STR_CAL_DICTATE_HINT);
  int y = top + 44;
  for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, hint, pageWidth - 2 * SIDE - 12, 6)) {
    renderer.drawText(UI_10_FONT_ID, SIDE + 6, y, line.c_str());
    y += 26;
  }
}

// La hora, elegida con la palanca. "[14]:30" dice qué pedazo se está moviendo:
// en blanco y negro es lo más claro que hay.
void CalendarActivity::renderTimeEditor() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + 24;
  if (menuItem >= 0 && menuItem < static_cast<int>(dayItems.size())) {
    renderer.drawCenteredText(
        UI_10_FONT_ID, top,
        renderer.truncatedText(UI_10_FONT_ID, dayItems[menuItem].title.c_str(), pageWidth - 2 * SIDE).c_str());
  }
  std::string label;
  if (editHour < 0) {
    label = std::string("[") + tr(STR_REM_NO_TIME) + "]";
  } else {
    label = (editMinuteField ? twoDigits(editHour) : "[" + twoDigits(editHour) + "]") + ":" +
            (editMinuteField ? "[" + twoDigits(editMinute) + "]" : twoDigits(editMinute));
  }
  renderer.drawCenteredText(UI_12_FONT_ID, top + 60, label.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, top + 110,
                            renderer.truncatedText(SMALL_FONT_ID, tr(STR_REM_FIELD_HINT), pageWidth - 2 * SIDE).c_str());
}

void CalendarActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  std::string title = tr(STR_CAL_TITLE);
  if (state == HOME) {
    title = tr(STR_HUB_DAY);
  } else if (state == TODAY) {
    title = tr(STR_DAY_TODAY);
  } else if (state == DICTATING) {
    title = recordMode == REC_TITLE ? tr(STR_CAL_ITEM_TITLE) : tr(STR_CAL_DICTATE);
  } else if (state == TIME_EDIT) {
    title = tr(STR_CAL_ITEM_TIME);
  } else if (state == DAY && viewYear != 0) {
    title = std::string(weekdayName(weekdayOfCivil(viewYear, viewMonth, cursorDay))) + " " +
            std::to_string(cursorDay) + " " + monthName(viewMonth);
  } else if (viewYear != 0) {
    title = std::string(monthName(viewMonth)) + " " + std::to_string(viewYear);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  switch (state) {
    case HOME:
      renderHome();
      break;
    case TODAY:
      renderToday();
      break;
    case MONTH:
      renderMonth();
      break;
    case DAY:
      renderDay();
      break;
    case DICTATING:
      renderDictating();
      break;
    case TIME_EDIT:
      renderTimeEditor();
      break;
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 10,
                                pending == DICTATE_SEND || pending == TITLE_SEND || afterWifi != NONE
                                    ? tr(STR_CAL_DICTATE_SENDING)
                                    : tr(STR_CAL_LOADING),
                                true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
      break;
  }

  // El menú de la actividad se dibuja encima de todo y se queda con los botones.
  if (menuOpen && menu.processRender(renderer, mappedInput)) return;
  // En Hoy, OK es lo único que le pide sugerencias al servidor, así que lo dice.
  const char* okLabel = state == DAY       ? tr(STR_CAL_DICTATE)
                        : state == TODAY   ? (suggestLines.empty() ? tr(STR_SUGGEST_ASK) : tr(STR_SUGGEST_REDO))
                                           : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), okLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Regla del panel: refresco limpio cada 10-15 parciales o la cuadrícula fantasmea.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
