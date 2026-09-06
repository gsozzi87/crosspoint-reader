#pragma once

#include <ServerClient.h>

#include "activities/Activity.h"

// Brings WiFi up, fetches GET /api/hub into HubStore (the SD cache the hub
// renders from), sets the RTC from the server clock when it is off, replays
// the offline queue, then silent-restarts back to the hub. Started by the hub
// itself (stale cache, or Back held) and from Settings -> Sync hub.
class HubSyncActivity final : public Activity {
 public:
  explicit HubSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HubSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

 private:
  enum State { CONNECTING, SYNCING, DONE, FAILED };
  State state = CONNECTING;
  ServerClient::Result result = ServerClient::Result::Transport;
  int status = 0;
  int flushed = 0;
  unsigned long doneAt = 0;

  void onWifiSelectionComplete(bool connected);
  void runSync();
  void markAttempt(bool ok);
};
