#pragma once

#include <string>
#include <utility>

#include "activities/Activity.h"
#include "activities/home/HubSyncActivity.h"
#include "voice/VoiceRecorder.h"

// Servicios privilegiados para apps Lua de fábrica. La app conserva toda la
// interfaz; esta pantalla transitoria solo posee WiFi, voz y escritura de
// libros. El nombre de la acción ya fue validado por LuaApp.
class LuaServiceActivity final : public Activity {
 public:
  LuaServiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string action)
      : Activity("LuaService", renderer, mappedInput), action(std::move(action)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != DONE && state != FAILED; }

 private:
  enum State { RECORDING, CONNECTING, WORKING, DONE, FAILED };
  State state = CONNECTING;
  std::string action;
  std::string topic;
  std::string bookPath;
  std::string detail;
  VoiceRecorder recorder{20};
  FriendlyWifi wifi;
  bool wifiPicker = false;
  bool pending = false;

  void beginConnect();
  void pumpConnect();
  void onConnected(bool ok);
  void runAction();
  bool syncTravel();
  bool makeResearchBook();
  void fail(std::string why);
};
