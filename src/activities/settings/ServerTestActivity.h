#pragma once

#include <ServerClient.h>

#include <string>

#include "activities/Activity.h"

// Diagnostic for the device's own server: brings WiFi up, checks that the
// server answers (public /firmware/latest), that the device token is accepted
// (/api/ping), and replays whatever the offline queue holds. Exercises the
// same ServerClient every server-backed feature uses, so a failure here is a
// configuration or server problem, not a feature bug.
class ServerTestActivity final : public Activity {
 public:
  explicit ServerTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ServerTest", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == CHECKING; }

 private:
  enum State { CONNECTING, CHECKING, DONE, WIFI_FAILED };
  State state = CONNECTING;

  std::string baseUrl;
  ServerClient::Result reach = ServerClient::Result::Transport;
  int reachStatus = 0;
  ServerClient::Result auth = ServerClient::Result::Transport;
  int authStatus = 0;
  size_t queued = 0;
  int flushed = 0;
  // El log sube acá a propósito: hasta 1.5.49 sólo salía con la sincronización
  // del hub (cada 6 h), así que para diagnosticar algo había que esperar o
  // forzar una sincronización entera. Esta pantalla ya levanta el WiFi y ya es
  // "el diagnóstico", así que es el lugar natural para mandarlo a pedido.
  bool logSent = false;
  size_t logBytes = 0;

  void onWifiSelectionComplete(bool connected);
  void runChecks();
};
