#pragma once

#include <ServerClient.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

class GfxRenderer;

// Conexion WiFi amigable: prueba las redes guardadas SIN ceder la pantalla, asi
// el usuario ve un cartel con el paso en el que va ("Conectando al WiFi...",
// "Buscando tu red...") en vez de la pantalla tecnica de seleccion, con SSIDs,
// intentos y errores crudos. Solo cuando ninguna red guardada sirve la Activity
// cae en WifiSelectionActivity (ahi si el usuario tiene que elegir a mano).
//
// No bloquea: se arranca con begin() y se le da cuerda desde el loop() de la
// Activity con pump(). revision() sube cuando cambio algo dibujable, para
// repintar solo ahi (el panel no aguanta un repintado por vuelta del loop).
class FriendlyWifi {
 public:
  enum class Phase {
    Idle,         // sin arrancar
    Connecting,   // probando una red guardada
    Searching,    // buscando cual de las guardadas esta al alcance
    Connected,    // listo
    NeedsPicker,  // ninguna red guardada sirve: que elija el usuario
  };

  void begin();
  Phase pump();
  Phase phase() const { return currentPhase; }
  bool isDone() const { return currentPhase == Phase::Connected || currentPhase == Phase::NeedsPicker; }
  uint32_t revision() const { return rev; }
  // Texto del paso actual, sin SSIDs ni jerga.
  const char* statusText() const;
  int progressPercent() const;
  // Cartel centrado: paso actual, barra de progreso y una linea de calma.
  static void drawStatus(const GfxRenderer& renderer, const FriendlyWifi& wifi, int centerY);

 private:
  Phase currentPhase = Phase::Idle;
  uint32_t rev = 0;
  int attempts = 0;
  bool scanDone = false;
  unsigned long attemptStartedAt = 0;
  unsigned long lastTick = 0;
  int ticks = 0;  // avance de la barra dentro de un intento
  std::string currentSsid;
  std::vector<std::string> tried;
  std::vector<std::string> candidates;  // guardadas y a la vista, la mas fuerte primero

  void bump() { rev++; }
  bool tryNetwork(const std::string& ssid);
  void startScan();
  void collectScanResults(int found);
  void nextCandidate();
  void onConnected();
};

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

  // GET /api/hub into HubStore with WiFi already up (also sets the RTC from the
  // server clock and stamps the attempt). Shared with the voice flow, which
  // refreshes the widgets right after an action. Returns true on success.
  static bool fetchNow(ServerClient::Result* resultOut = nullptr, int* statusOut = nullptr);
  // Aplica el idioma pedido desde /board (HubStore::uiLang) a los ajustes del
  // lector. Se llama sola al terminar una sincronización.
  static void applyUiLanguage();

 private:
  enum State { CONNECTING, SYNCING, DONE, FAILED };
  State state = CONNECTING;
  ServerClient::Result result = ServerClient::Result::Transport;
  int status = 0;
  int flushed = 0;
  unsigned long doneAt = 0;
  FriendlyWifi wifi;
  bool wifiPicker = false;  // la pantalla de seleccion tiene el foco
  bool wifiFailed = false;  // fallo la red, no el servidor: sin detalle tecnico

  void beginConnect();
  void pumpConnect();
  void onWifiSelectionComplete(bool connected);
  void runSync();
  // De que cuenta es el aparato ahora: si cambio, se tira la cola y la cache.
  void checkAccount();
  static void markAttempt(bool ok);
  // GET /api/tts for the next reminders ("Reminders: <title>") and the timer
  // phrase, saved as /.crosspoint/tts/*.bin so the alerts speak without WiFi.
  static void cacheSpokenNotices();
};
