#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "WEATHER";
constexpr const char* CACHE = "/.crosspoint/forecast.json";
constexpr unsigned long REFRESH_HOLD_MS = 1200;
constexpr time_t CACHE_MAX_AGE_S = 3600;  // más viejo que esto: refrescar al entrar
constexpr int SIDE = 20;

void drawSdkIcon(const GfxRenderer& renderer, const freeink::Icon& icon, int x, int y, bool ink = true) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* line = icon.bits + row * stride;
    for (int col = 0; col < icon.w; ++col) {
      if ((line[col / 8] & (0x80 >> (col % 8))) == 0) renderer.drawPixel(x + col, y + row, ink);
    }
  }
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
  sunrise = doc["sunrise"] | "";
  sunset = doc["sunset"] | "";
  hours.clear();
  for (JsonVariantConst h : doc["hours"].as<JsonArrayConst>()) {
    hours.push_back({h["h"] | "", h["t"] | 0, h["p"] | 0, h["c"] | ""});
  }
  days.clear();
  for (JsonVariantConst d : doc["days"].as<JsonArrayConst>()) {
    days.push_back({d["d"] | "", d["date"] | "", d["max"] | 0, d["min"] | 0, d["p"] | 0, d["c"] | ""});
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
    return false;
  }
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
  } else {
    int y = metrics.topPadding + metrics.headerHeight + 10;
    const int w = pageWidth - 2 * SIDE;
    char line[128];

    // Now: temperature big on the left, condition and the rest stacked right.
    char big[16];
    snprintf(big, sizeof(big), "%d°", nowTemp);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 6, big, true, EpdFontFamily::BOLD);
    const int bigW = renderer.getTextWidth(UI_12_FONT_ID, big, EpdFontFamily::BOLD);
    const int tx = SIDE + bigW + 14;
    renderer.drawText(UI_10_FONT_ID, tx, y, renderer.truncatedText(UI_10_FONT_ID, nowCond.c_str(), pageWidth - SIDE - tx).c_str(), true, EpdFontFamily::BOLD);
    snprintf(line, sizeof(line), "%s %d°  ·  %s %d%%", tr(STR_WEATHER_FEELS), feels, tr(STR_WEATHER_HUM), hum);
    renderer.drawText(SMALL_FONT_ID, tx, y + 22, line);
    snprintf(line, sizeof(line), "%s %d km/h", tr(STR_WEATHER_WIND), wind);
    if (!sunrise.empty()) {
      snprintf(line + strlen(line), sizeof(line) - strlen(line), "  ·  %s %s  %s %s", tr(STR_WEATHER_SUNRISE),
               sunrise.c_str(), tr(STR_WEATHER_SUNSET), sunset.c_str());
    }
    renderer.drawText(SMALL_FONT_ID, tx, y + 40, renderer.truncatedText(SMALL_FONT_ID, line, pageWidth - SIDE - tx).c_str());
    y += 66;

    // Hours: two rows of four columns, each with time, temperature and rain.
    if (!hours.empty()) {
      renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
      y += 8;
      const int cols = 4;
      const int colW = w / cols;
      for (size_t i = 0; i < hours.size() && i < 8; ++i) {
        const int cx = SIDE + static_cast<int>(i % cols) * colW;
        const int cy = y + static_cast<int>(i / cols) * 48;
        const Hour& h = hours[i];
        renderer.drawText(SMALL_FONT_ID, cx, cy, h.at.c_str());
        snprintf(line, sizeof(line), "%d°", h.temp);
        renderer.drawText(UI_10_FONT_ID, cx, cy + 16, line, true, EpdFontFamily::BOLD);
        if (h.rain >= 20) {
          snprintf(line, sizeof(line), "%d%%", h.rain);
          renderer.drawText(SMALL_FONT_ID, cx + 34, cy + 20, line);
        }
      }
      y += hours.size() > 4 ? 100 : 52;
    }

    // Days: one row each — name, date, condition, rain, and the min/max bar.
    renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
    y += 6;
    int gmin = 99, gmax = -99;
    for (const Day& d : days) {
      gmin = std::min(gmin, d.min);
      gmax = std::max(gmax, d.max);
    }
    if (gmax <= gmin) gmax = gmin + 1;
    const int avail = pageHeight - metrics.buttonHintsHeight - 10 - y;
    const int rowH = days.empty() ? 0 : std::min(56, avail / static_cast<int>(days.size()));
    for (size_t i = 0; i < days.size(); ++i) {
      const Day& d = days[i];
      const int ry = y + static_cast<int>(i) * rowH;
      const bool today = i == 0;
      renderer.drawText(UI_10_FONT_ID, SIDE, ry + 2, d.name.c_str(), true,
                        today ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, SIDE + 54, ry + 4, d.date.c_str());
      // Condition, and the rain chance right after it
      std::string cond = d.cond;
      if (d.rain >= 20) cond += "  " + std::to_string(d.rain) + "%";
      renderer.drawText(SMALL_FONT_ID, SIDE + 104, ry + 4,
                        renderer.truncatedText(SMALL_FONT_ID, cond.c_str(), w - 104 - 4).c_str());
      // Bar with the min/max of the week, temperatures at both ends
      const int barY = ry + 24;
      snprintf(line, sizeof(line), "%d°", d.min);
      const int minW = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, SIDE, barY - 2, line);
      snprintf(line, sizeof(line), "%d°", d.max);
      const int maxW = renderer.getTextWidth(UI_10_FONT_ID, line, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - maxW, barY - 6, line, true, EpdFontFamily::BOLD);
      const int barX = SIDE + minW + 8;
      const int barW = pageWidth - SIDE - maxW - 8 - barX;
      if (barW > 20) {
        renderer.drawLine(barX, barY + 4, barX + barW, barY + 4, true);
        const int x0 = barX + barW * (d.min - gmin) / (gmax - gmin);
        const int x1 = barX + barW * (d.max - gmin) / (gmax - gmin);
        renderer.fillRoundedRect(x0, barY, std::max(x1 - x0, 6), 9, 4, Color::Black);
      }
      if (i + 1 < days.size()) renderer.drawLine(SIDE, ry + rowH - 4, pageWidth - SIDE, ry + rowH - 4, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
