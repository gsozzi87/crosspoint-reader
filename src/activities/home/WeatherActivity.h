#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

// Detailed weather: now (temperature, feels like, humidity, wind), the next
// hours and six days, from GET /api/hub/forecast. Reached from the hub's
// weather widget (OK on the hub). The last forecast is cached on the SD so it
// opens offline; Back held refreshes.
class WeatherActivity final : public Activity {
 public:
  explicit WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

 private:
  enum State { SHOW, CONNECTING, LOADING, FAILED };
  State state = SHOW;

  struct Hour {
    std::string at;
    int temp = 0;
    int rain = 0;
    std::string cond;
  };
  struct Day {
    std::string name;
    std::string date;
    int max = 0;
    int min = 0;
    int rain = 0;
    std::string cond;
  };

  std::string place;
  std::string nowCond;
  int nowTemp = 0, feels = 0, hum = 0, wind = 0;
  std::string sunrise, sunset;
  std::vector<Hour> hours;
  std::vector<Day> days;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;

  bool parse(const std::string& json);
  bool loadCache();
  bool fetchForecast();
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
};
