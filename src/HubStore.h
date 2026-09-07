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
    int id = 0;
    std::string from;
    std::string text;
  };
  struct Reminder {
    int id = 0;
    std::string title;
    std::string when;
    time_t dueAt = 0;  // UTC epoch, 0 = no time
  };
  struct ListItem {
    int id = 0;
    std::string text;
  };
  struct List {
    std::string name;
    std::vector<ListItem> items;  // pending only
  };
  struct Note {
    int id = 0;
    std::string text;
  };

  static constexpr int MAX_EVENTS = 4;
  static constexpr int MAX_MESSAGES = 5;
  static constexpr int MAX_REMINDERS = 20;
  static constexpr int MAX_LISTS = 12;
  static constexpr int MAX_ITEMS = 30;
  static constexpr int MAX_NOTES = 20;

  time_t syncedAt = 0;       // UTC epoch of the last successful sync (0 = never)
  time_t lastAttemptAt = 0;  // UTC epoch of the last attempt, successful or not (1 = attempted, no clock)
  std::string weatherLine;   // "Nublado · 18°"
  std::string weatherDetail; // "Máx 22° · Mín 11° · Humedad 60 %"
  bool weatherNoPlace = false;  // el servidor no tiene lugar cargado: hay que ponerlo en /board
  std::string weatherError;     // motivo que manda el servidor cuando el clima falló (Open-Meteo caído, 429...)
  std::string reminderTitle;  // = reminders[0], kept for the widget
  std::string reminderWhen;
  std::vector<Reminder> reminders;
  std::vector<List> lists;
  std::vector<Note> notes;
  std::vector<Event> events;
  std::vector<Message> messages;
  std::string quote;
  std::string verseRef;   // verse of the day (Bible), shown alternating with the quote
  std::string verseText;
  std::string translatorLang;  // the other side of the translator ("en", ...), remembered
  int bibleBook = 0;     // last place read in the Bible (book index, chapter 1-based)
  int bibleChapter = 0;
  int musicVolume = 70;  // MP3 player volume, 0-100
  // Temporizador / pomodoro / cronómetro: sobreviven al sueño (el aparato se
  // despierta para el temporizador) y se ven en el hub. Salir con Atrás NO los
  // cancela: siguen corriendo hasta que suenan o se cancelan con Atrás largo.
  time_t timerEndAt = 0;   // epoch UTC en que suena, 0 = no hay cuenta regresiva corriendo
  int timerTotal = 0;      // segundos del tramo, para la línea de progreso
  uint8_t timerMode = 0;   // 0 cuenta regresiva, 1 pomodoro trabajo, 2 pomodoro descanso
  int timerPausedLeft = 0; // segundos que quedaban al pausar (0 = no hay nada pausado)
  uint8_t timerRound = 1;  // ronda del pomodoro, para no volver a "Ronda 1" al retomar
  time_t stopwatchStartAt = 0;  // epoch UTC del arranque del tramo actual del cronómetro (0 = no corre)
  int stopwatchAccumS = 0;      // segundos acumulados de tramos anteriores del cronómetro
  bool timerRunning() const { return timerEndAt > 0; }
  bool timerPaused() const { return timerEndAt == 0 && timerPausedLeft > 0; }
  bool stopwatchActive() const { return stopwatchStartAt > 0 || stopwatchAccumS > 0; }
  long stopwatchElapsed(time_t now) const {
    return stopwatchAccumS + (stopwatchStartAt > 0 && now > stopwatchStartAt ? static_cast<long>(now - stopwatchStartAt) : 0);
  }
  // Algo que mostrar en el hub (temporizador corriendo, pausado o cronómetro).
  bool timeActive() const { return timerRunning() || timerPaused() || stopwatchActive(); }
  void clearTimer() {
    timerEndAt = 0;
    timerPausedLeft = 0;
    timerTotal = 0;
    timerMode = 0;
    timerRound = 1;
  }
  void clearStopwatch() {
    stopwatchStartAt = 0;
    stopwatchAccumS = 0;
  }
  uint8_t speakMode = 1;       // spoken replies: 0 never, 1 short ones, 2 always (Settings)
  // Ajustes cargados en /board. El servidor manda `settings.rev`; solo se
  // aplican cuando esa revisión es mayor a la última aplicada, así lo que se
  // cambia en el aparato no se pisa en cada sincronización.
  int settingsRev = 0;
  std::string uiLang;          // idioma pedido desde la web ("es", "en", ...); lo aplica HubSyncActivity
  std::string ttsVoice;        // voz de Piper del servidor: entra en el nombre de los clips cacheados
  const char* speakParam() const { return speakMode == 0 ? "none" : speakMode == 2 ? "all" : "short"; }

  static const char* getFilePath() { return "/.crosspoint/hub.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Replaces the cached content with a server payload ({weather, reminders, events, messages, quote}).
  void applyServer(JsonVariantConst doc);
  // Ajustes de /board dentro de esa respuesta; solo si `rev` subió.
  void applySettings(JsonVariantConst settings);
  bool hasSynced() const { return syncedAt > 0; }
  // Local tick (the server gets POST /api/hub/done from the caller, queued if offline).
  void removeReminder(int id);
  void removeItem(int id);
  // Earliest dueAt in the future (or 0): what the deep-sleep timer is armed to.
  time_t nextDueAt(time_t now) const;
  // First reminder whose time has come (dueAt <= now), or nullptr.
  const Reminder* dueReminder(time_t now) const;
  void snoozeReminder(int id, time_t until);
  void removeNote(int id);
  void removeMessage(int id);
  void moveItem(int id, const std::string& listName);

 private:
  HubStore() = default;
  ~HubStore() = default;
  friend class PersistableStore<HubStore>;
};

#define HUB_STORE HubStore::getInstance()
