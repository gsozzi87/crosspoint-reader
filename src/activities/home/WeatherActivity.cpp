#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
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
  if (loadCache()) {
    state = SHOW;
    requestUpdate();
  } else {
    ensureConnected();
  }
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
  if (SERVER_CLIENT.get(std::string("/api/hub/forecast?lang=") + uiLanguageCode(), resp) != ServerClient::Result::Ok) {
    return false;
  }
  if (!parse(resp.body)) return false;
  HalFile f;
  if (Storage.openFileForWrite(TAG, CACHE, f)) {
    f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
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
        failureId = StrId::STR_ASK_FAILED;
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
    int y = metrics.topPadding + metrics.headerHeight + 14;
    // Now: big temperature, condition and the rest in one line.
    char big[16];
    snprintf(big, sizeof(big), "%d°", nowTemp);
    renderer.drawText(UI_12_FONT_ID, SIDE, y, big, true, EpdFontFamily::BOLD);
    const int bigW = renderer.getTextWidth(UI_12_FONT_ID, big, EpdFontFamily::BOLD);
    drawSdkIcon(renderer, icon_hub_weather_24, SIDE + bigW + 12, y - 2);
    renderer.drawText(UI_12_FONT_ID, SIDE + bigW + 44, y, renderer.truncatedText(UI_12_FONT_ID, nowCond.c_str(), pageWidth - SIDE * 2 - bigW - 44).c_str(), true, EpdFontFamily::BOLD);
    y += 28;
    char line[128];
    snprintf(line, sizeof(line), "%s %d°   %s %d %%   %s %d km/h", tr(STR_WEATHER_FEELS), feels, tr(STR_WEATHER_HUM), hum,
             tr(STR_WEATHER_WIND), wind);
    renderer.drawText(UI_10_FONT_ID, SIDE, y, line);
    y += 24;
    if (!sunrise.empty()) {
      snprintf(line, sizeof(line), "%s %s   %s %s", tr(STR_WEATHER_SUNRISE), sunrise.c_str(), tr(STR_WEATHER_SUNSET), sunset.c_str());
      renderer.drawText(SMALL_FONT_ID, SIDE, y, line);
      y += 22;
    }

    // Hours: a row of columns with time, temperature and rain chance.
    if (!hours.empty()) {
      renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
      y += 10;
      const int cols = static_cast<int>(hours.size());
      const int colW = (pageWidth - 2 * SIDE) / cols;
      for (int i = 0; i < cols; ++i) {
        const int cx = SIDE + i * colW;
        const Hour& h = hours[i];
        renderer.drawText(SMALL_FONT_ID, cx, y, h.at.c_str());
        snprintf(line, sizeof(line), "%d°", h.temp);
        renderer.drawText(UI_10_FONT_ID, cx, y + 18, line, true, EpdFontFamily::BOLD);
        if (h.rain >= 20) {
          snprintf(line, sizeof(line), "%d%%", h.rain);
          renderer.drawText(SMALL_FONT_ID, cx, y + 40, line);
        }
      }
      y += 62;
    }

    // Days: name, condition, rain chance and max/min with a simple range bar.
    renderer.drawLine(SIDE, y, pageWidth - SIDE, y, true);
    y += 8;
    int gmin = 99, gmax = -99;
    for (const Day& d : days) {
      gmin = std::min(gmin, d.min);
      gmax = std::max(gmax, d.max);
    }
    if (gmax <= gmin) gmax = gmin + 1;
    const int rowH = std::max(26, (pageHeight - metrics.buttonHintsHeight - 14 - y) / static_cast<int>(days.size()));
    for (size_t i = 0; i < days.size(); ++i) {
      const Day& d = days[i];
      const int ry = y + static_cast<int>(i) * rowH;
      renderer.drawText(UI_10_FONT_ID, SIDE, ry + 4, d.name.c_str(), true, i == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, SIDE + 60, ry + 6, d.date.c_str());
      renderer.drawText(UI_10_FONT_ID, SIDE + 110, ry + 4, renderer.truncatedText(UI_10_FONT_ID, d.cond.c_str(), 150).c_str());
      if (d.rain >= 20) {
        snprintf(line, sizeof(line), "%d%%", d.rain);
        renderer.drawText(SMALL_FONT_ID, SIDE + 262, ry + 6, line);
      }
      // Range bar between the week's min and max
      const int barX = SIDE + 320, barW = pageWidth - SIDE - 80 - barX;
      snprintf(line, sizeof(line), "%d°", d.min);
      renderer.drawText(SMALL_FONT_ID, barX - renderer.getTextWidth(SMALL_FONT_ID, line) - 8, ry + 6, line);
      const int x0 = barX + barW * (d.min - gmin) / (gmax - gmin);
      const int x1 = barX + barW * (d.max - gmin) / (gmax - gmin);
      renderer.drawLine(barX, ry + 12, barX + barW, ry + 12, true);
      renderer.fillRoundedRect(x0, ry + 8, std::max(x1 - x0, 6), 9, 4, Color::Black);
      snprintf(line, sizeof(line), "%d°", d.max);
      renderer.drawText(UI_10_FONT_ID, barX + barW + 10, ry + 4, line, true, EpdFontFamily::BOLD);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
