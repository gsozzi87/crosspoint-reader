#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "util/Shtc3.h"

#include <cmath>
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "components/icons/weatherIcons.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "WEATHER";
constexpr const char* CACHE = "/.crosspoint/forecast.json";
constexpr unsigned long REFRESH_HOLD_MS = 1200;
constexpr time_t CACHE_MAX_AGE_S = 3600;  // más viejo que esto: refrescar al entrar
constexpr int SIDE = 22;
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
constexpr int BIG_FONT_ID = NOTOSANS_18_FONT_ID;  // la temperatura de ahora, bien grande


// Dibujo para cada código WMO de Open-Meteo (los mismos tramos que usa el
// servidor en describeWeather()): 0 despejado, 1-2 algo nublado, 3 nublado,
// 45/48 niebla, 51-57 llovizna, 61-67 lluvia, 71-77 nieve, 80-82 chaparrones,
// 85-86 chaparrones de nieve, 95-99 tormenta.
const freeink::Icon& iconForWmo(const int code, const bool big) {
  if (code < 0) return big ? icon_wx_cloudy_64 : icon_wx_cloudy_36;   // sin dato: neutro
  if (code == 0) return big ? icon_wx_clear_64 : icon_wx_clear_36;
  if (code <= 2) return big ? icon_wx_partly_64 : icon_wx_partly_36;
  if (code == 3) return big ? icon_wx_cloudy_64 : icon_wx_cloudy_36;
  if (code <= 48) return big ? icon_wx_fog_64 : icon_wx_fog_36;
  if (code <= 57) return big ? icon_wx_drizzle_64 : icon_wx_drizzle_36;
  if (code <= 67) return big ? icon_wx_rain_64 : icon_wx_rain_36;
  if (code <= 77) return big ? icon_wx_snow_64 : icon_wx_snow_36;
  if (code <= 82) return big ? icon_wx_showers_64 : icon_wx_showers_36;
  if (code <= 86) return big ? icon_wx_snow_64 : icon_wx_snow_36;
  return big ? icon_wx_storm_64 : icon_wx_storm_36;
}

// El servidor manda la condición ya traducida y (todavía) no manda el código
// WMO, así que se vuelve del texto al código con la misma tabla de lang.ts: son
// nueve frases fijas por idioma (es, en, fr, de, pt, ru). Si algún día
// /api/hub/forecast agrega el número, gana el número y esto no se usa.
struct CondCode {
  const char* text;
  int code;
};
// El orden importa para la pasada por subcadena: primero lo específico
// ("Algo nublado" contiene "nublado", "Nieselregen" contiene "Regen").
const CondCode CONDS[] = {
    {"Algo nublado", 2},          {"Parcialmente nublado", 2}, {"Partly cloudy", 2},
    {"Peu nuageux", 2},           {"Leicht bewölkt", 2},       {"Малооблачно", 2},
    {"Llovizna", 51},             {"Drizzle", 51},             {"Bruine", 51},
    {"Nieselregen", 51},          {"Chuvisco", 51},            {"Морось", 51},
    {"Chaparrones", 80},          {"Showers", 80},             {"Averses", 80},
    {"Schauer", 80},              {"Pancadas", 80},            {"Ливни", 80},
    {"Tormenta", 95},             {"Thunderstorm", 95},        {"Orage", 95},
    {"Gewitter", 95},             {"Tempestade", 95},          {"Гроза", 95},
    {"Nieve", 71},                {"Snow", 71},                {"Neige", 71},
    {"Schnee", 71},               {"Neve", 71},                {"Снег", 71},
    {"Lluvia", 61},               {"Rain", 61},                {"Pluie", 61},
    {"Regen", 61},                {"Chuva", 61},               {"Дождь", 61},
    {"Niebla", 45},               {"Fog", 45},                 {"Brouillard", 45},
    {"Nebel", 45},                {"Névoa", 45},               {"Туман", 45},
    {"Nublado", 3},               {"Cloudy", 3},               {"Nuageux", 3},
    {"Bewölkt", 3},               {"Облачно", 3},              {"Despejado", 0},
    {"Clear", 0},                 {"Dégagé", 0},               {"Klar", 0},
    {"Céu limpo", 0},             {"Ясно", 0},
};

int wmoFromCondition(const std::string& cond) {
  if (cond.empty()) return -1;
  for (const CondCode& c : CONDS) {
    if (cond == c.text) return c.code;  // el caso normal: la frase entera
  }
  for (const CondCode& c : CONDS) {
    if (cond.find(c.text) != std::string::npos) return c.code;  // por las dudas, con sufijos
  }
  return -1;
}
}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  const bool haveCache = loadCache();
  time_t now = 0;
  const bool fresh = haveCache && cachedAt > 0 && halClock.getEpochUtc(now) && now - cachedAt < CACHE_MAX_AGE_S;
  if (haveCache) {
    state = SHOW;
    requestUpdate();
  }
  // Sin caché, o con una vieja, se pide de nuevo: si falla, queda lo cacheado.
  if (!fresh) ensureConnected();
}

void WeatherActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool WeatherActivity::parse(const std::string& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !(doc["ok"] | false)) return false;
  cachedAt = static_cast<time_t>(doc["savedAt"] | (int64_t)0);
  place = doc["place"] | "";
  nowTemp = doc["now"]["t"] | 0;
  feels = doc["now"]["feels"] | 0;
  hum = doc["now"]["hum"] | 0;
  wind = doc["now"]["wind"] | 0;
  nowCond = doc["now"]["c"] | "";
  nowCode = doc["now"]["w"] | -1;
  if (nowCode < 0) nowCode = wmoFromCondition(nowCond);
  sunrise = doc["sunrise"] | "";
  sunset = doc["sunset"] | "";
  hours.clear();
  for (JsonVariantConst h : doc["hours"].as<JsonArrayConst>()) {
    Hour item{h["h"] | "", h["t"] | 0, h["p"] | 0, h["w"] | -1, h["c"] | ""};
    if (item.code < 0) item.code = wmoFromCondition(item.cond);
    hours.push_back(item);
  }
  days.clear();
  for (JsonVariantConst d : doc["days"].as<JsonArrayConst>()) {
    Day item{d["d"] | "", d["date"] | "", d["max"] | 0, d["min"] | 0, d["p"] | 0, d["w"] | -1, d["c"] | ""};
    if (item.code < 0) item.code = wmoFromCondition(item.cond);
    days.push_back(item);
  }
  return !days.empty();
}

bool WeatherActivity::loadCache() {
  if (!Storage.exists(CACHE)) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, CACHE, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = f.read(&raw[0], raw.size());
  f.close();
  return got > 0 && parse(raw);
}

bool WeatherActivity::fetchForecast() {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get(std::string("/api/hub/forecast?lang=") + uiLanguageCode(), resp);
  lastStatus = resp.status;
  if (r != ServerClient::Result::Ok) {
    LOG_ERR(TAG, "GET /api/hub/forecast: %s (%d)", ServerClient::resultName(r), resp.status);
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    return false;
  }
  failureDetail.clear();
  if (!parse(resp.body)) {
    LOG_ERR(TAG, "bad forecast payload (%d bytes)", (int)resp.body.size());
    return false;
  }
  // Se guarda con la hora para saber después si la caché sirve o hay que pedir
  // de nuevo (el cuerpo siempre empieza con '{').
  std::string body = resp.body;
  time_t now = 0;
  if (halClock.getEpochUtc(now) && !body.empty() && body[0] == '{') {
    char stamp[40];
    snprintf(stamp, sizeof(stamp), "{\"savedAt\":%lld,", (long long)now);
    body = stamp + body.substr(1);
    cachedAt = now;
  }
  HalFile f;
  if (Storage.openFileForWrite(TAG, CACHE, f)) {
    f.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    f.close();
  }
  return true;
}

void WeatherActivity::ensureConnected() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeatherActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    failureId = StrId::STR_SERVER_WIFI_FAILED;
    failureDetail.clear();
    state = days.empty() ? FAILED : SHOW;
    requestUpdate();
    return;
  }
  state = LOADING;
  requestUpdate();
}

void WeatherActivity::loop() {
  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      const bool ok = fetchForecast();
      WiFi.setSleep(true);
      if (!ok && days.empty()) {
        // 503 del servidor = no hay lugar cargado: decirlo, no "no se pudo".
        failureId = lastStatus == 503 ? StrId::STR_HUB_NO_PLACE : StrId::STR_ASK_FAILED;
        state = FAILED;
      } else {
        state = SHOW;
      }
      requestUpdate();
      break;
    }
    case SHOW:
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, REFRESH_HOLD_MS)) {
        ensureConnected();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activityManager.goHome();
      }
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        activityManager.goHome();
      }
      break;
    case CONNECTING:
      break;
  }
}

void WeatherActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 place.empty() ? tr(STR_WEATHER_TITLE) : place.c_str());

  if (state == LOADING || state == CONNECTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_WEATHER_LOADING), true, EpdFontFamily::BOLD);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, I18N.get(failureId), true, EpdFontFamily::BOLD);
    // El motivo concreto: sin esto "no se pudo" no dice si es el WiFi, el token,
    // el servidor o que falta el lugar.
    if (!failureDetail.empty()) {
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 20,
                                renderer.truncatedText(SMALL_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
    }
    const std::string base = SERVER_STORE.getBaseUrl();
    renderer.drawCenteredText(SMALL_FONT_ID, mid + 46,
                              base.empty() ? tr(STR_SERVER_NOT_CONFIGURED)
                                           : renderer.truncatedText(SMALL_FONT_ID, base.c_str(), pageWidth - 40).c_str());
  } else {
    int y = metrics.topPadding + metrics.headerHeight + 12;
    const int w = pageWidth - 2 * SIDE;
    const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
    const int ui10H = renderer.getLineHeight(UI_10_FONT_ID);
    const int ui12H = renderer.getLineHeight(UI_12_FONT_ID);
    const int bigH = renderer.getLineHeight(BIG_FONT_ID);
    char line[160];
    char temp[16];

    // ---- Ahora: el dibujo grande a la izquierda, la temperatura al lado ----
    const freeink::Icon& nowIcon = iconForWmo(nowCode, true);
    BaseTheme::drawIconBitmap(renderer, nowIcon, SIDE, y);
    const int tx = SIDE + nowIcon.w + 20;
    snprintf(temp, sizeof(temp), "%d°", nowTemp);
    renderer.drawText(BIG_FONT_ID, tx, y, temp, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, tx, y + bigH - 4,
                      renderer.truncatedText(UI_10_FONT_ID, nowCond.c_str(), pageWidth - SIDE - tx).c_str(), true,
                      EpdFontFamily::BOLD);
    y += std::max<int>(nowIcon.h, bigH + ui10H - 4) + 14;

    // El detalle en dos renglones chicos, a todo el ancho útil.
    snprintf(line, sizeof(line), "%s %d°   ·   %s %d%%   ·   %s %d km/h", tr(STR_WEATHER_FEELS), feels,
             tr(STR_WEATHER_HUM), hum, tr(STR_WEATHER_WIND), wind);
    renderer.drawText(SMALL_FONT_ID, SIDE, y, renderer.truncatedText(SMALL_FONT_ID, line, w).c_str());
    y += smallH + 4;
    if (!sunrise.empty()) {
      snprintf(line, sizeof(line), "%s %s   ·   %s %s", tr(STR_WEATHER_SUNRISE), sunrise.c_str(),
               tr(STR_WEATHER_SUNSET), sunset.c_str());
      renderer.drawText(SMALL_FONT_ID, SIDE, y, renderer.truncatedText(SMALL_FONT_ID, line, w).c_str());
      y += smallH + 4;
    }
    // Adentro: lo único de esta pantalla que no viene del servidor. Sale del
    // SHTC3 de la placa y es, en la práctica, con lo que uno decide si prende
    // la estufa.
    const float inside = shtc3::cachedCelsius();
    if (!std::isnan(inside)) {
      const float insideRh = shtc3::cachedHumidity();
      if (!std::isnan(insideRh)) {
        snprintf(line, sizeof(line), "%s %d°   ·   %d%%", tr(STR_HUB_INDOOR), static_cast<int>(inside + 0.5f),
                 static_cast<int>(insideRh + 0.5f));
      } else {
        snprintf(line, sizeof(line), "%s %d°", tr(STR_HUB_INDOOR), static_cast<int>(inside + 0.5f));
      }
      renderer.drawText(SMALL_FONT_ID, SIDE, y, renderer.truncatedText(SMALL_FONT_ID, line, w).c_str());
      y += smallH + 4;
    }
    y += 10;

    // ---- Próximas horas: cuatro columnas con hora, dibujo y temperatura ----
    if (!hours.empty()) {
      renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
      y += 12;
      const int cols = 4;
      const int colW = w / cols;
      const int iconY = y + smallH + 4;
      const int tempY = iconY + icon_wx_cloudy_36.h + 6;
      const int shown = std::min<int>(cols, static_cast<int>(hours.size()));
      bool anyRain = false;
      for (int i = 0; i < shown; ++i) anyRain = anyRain || hours[i].rain >= 20;
      for (int i = 0; i < shown; ++i) {
        const Hour& h = hours[i];
        const int cc = SIDE + i * colW + colW / 2;  // centro de la columna
        int tw = renderer.getTextWidth(SMALL_FONT_ID, h.at.c_str());
        renderer.drawText(SMALL_FONT_ID, cc - tw / 2, y, h.at.c_str());
        const freeink::Icon& ic = iconForWmo(h.code, false);
        BaseTheme::drawIconBitmap(renderer, ic, cc - ic.w / 2, iconY);
        snprintf(temp, sizeof(temp), "%d°", h.temp);
        tw = renderer.getTextWidth(UI_10_FONT_ID, temp, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, cc - tw / 2, tempY, temp, true, EpdFontFamily::BOLD);
        if (h.rain >= 20) {
          snprintf(line, sizeof(line), "%d%%", h.rain);
          tw = renderer.getTextWidth(SMALL_FONT_ID, line);
          renderer.drawText(SMALL_FONT_ID, cc - tw / 2, tempY + ui10H + 2, line);
        }
      }
      // El renglón de lluvia solo ocupa lugar si alguna hora lo usa.
      y = tempY + ui10H + 2 + (anyRain ? smallH : 0) + 14;
    }

    // ---- Los días: un dibujo por fila, el día y máxima/mínima ----
    if (!days.empty()) {
      renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
      y += 8;
      const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int avail = bottom - y;
      int rowH = avail / static_cast<int>(days.size());
      rowH = std::max(44, std::min(rowH, 84));  // que respire, pero sin pasarse
      for (size_t i = 0; i < days.size(); ++i) {
        const Day& d = days[i];
        const int ry = y + static_cast<int>(i) * rowH;
        if (ry + rowH > bottom) break;  // nunca por encima de la barra de botones
        const int center = ry + rowH / 2;
        const freeink::Icon& ic = iconForWmo(d.code, false);
        BaseTheme::drawIconBitmap(renderer, ic, SIDE, center - ic.opticalCenterY);

        // Máxima (grande) y mínima (chica) pegadas al borde derecho.
        snprintf(temp, sizeof(temp), "%d°", d.min);
        const int minW = renderer.getTextWidth(UI_10_FONT_ID, temp);
        const int minX = pageWidth - SIDE - minW;
        renderer.drawText(UI_10_FONT_ID, minX, center - ui10H / 2, temp);
        snprintf(temp, sizeof(temp), "%d°", d.max);
        const int maxW = renderer.getTextWidth(UI_12_FONT_ID, temp, EpdFontFamily::BOLD);
        const int maxX = minX - 16 - maxW;
        renderer.drawText(UI_12_FONT_ID, maxX, center - ui12H / 2, temp, true, EpdFontFamily::BOLD);

        // El día arriba del centro y la fecha (con la lluvia si vale la pena)
        // abajo, siempre recortados contra el hueco que queda libre.
        const int nameX = SIDE + ic.w + 18;
        const int textW = maxX - 12 - nameX;
        const bool twoLines = rowH >= ui10H + smallH + 4;  // si la fila no da, solo el día
        if (textW > 24) {
          const EpdFontFamily::Style nameStyle = i == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
          const int nameY = twoLines ? center - ui10H - 1 : center - ui10H / 2;
          renderer.drawText(UI_10_FONT_ID, nameX, nameY,
                            renderer.truncatedText(UI_10_FONT_ID, d.name.c_str(), textW, nameStyle).c_str(), true,
                            nameStyle);
          if (twoLines) {
            std::string sub = d.date;
            if (d.rain >= 20) sub += "   " + std::to_string(d.rain) + "%";
            renderer.drawText(SMALL_FONT_ID, nameX, center + 1,
                              renderer.truncatedText(SMALL_FONT_ID, sub.c_str(), textW).c_str());
          }
        }
        if (i + 1 < days.size() && ry + 2 * rowH <= bottom) {
          renderer.drawLine(SIDE, ry + rowH, pageWidth - SIDE, ry + rowH, true);
        }
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Regla del panel: refresco limpio cada 10-15 parciales o la pantalla fantasmea.
  const bool clean = ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) partialCount = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
