#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <ctime>
#include <string>
#include <vector>

// What the hub shows when there is no WiFi: the last GET /api/hub, cached on
// the SD card. HubSyncActivity refreshes it; HubActivity only reads it.
class HubStore : public PersistableStore<HubStore> {
 public:
  struct Event {
    std::string when;  // "10:30", "mañana 9:00"; already formatted by the server
    std::string title;
  };
  struct Message {
    std::string from;
    std::string text;
  };

  static constexpr int MAX_EVENTS = 4;
  static constexpr int MAX_MESSAGES = 5;

  time_t syncedAt = 0;       // UTC epoch of the last successful sync (0 = never)
  time_t lastAttemptAt = 0;  // UTC epoch of the last attempt, successful or not (1 = attempted, no clock)
  std::string weatherLine;   // "Nublado · 18°"
  std::string weatherDetail; // "Máx 22° · Mín 11° · Humedad 60 %"
  std::string reminderTitle;
  std::string reminderWhen;
  std::vector<Event> events;
  std::vector<Message> messages;
  std::string quote;

  static const char* getFilePath() { return "/.crosspoint/hub.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Replaces the cached content with a server payload ({weather, reminders, events, messages, quote}).
  void applyServer(JsonVariantConst doc);
  bool hasSynced() const { return syncedAt > 0; }

 private:
  HubStore() = default;
  ~HubStore() = default;
  friend class PersistableStore<HubStore>;
};

#define HUB_STORE HubStore::getInstance()
