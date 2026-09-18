#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/home/HubSyncActivity.h"  // FriendlyWifi: conexión sin la pantalla técnica
#include "lua/LuaApp.h"
#include "util/ButtonNavigator.h"
#include "voice/VoiceRecorder.h"

// El catálogo de apps de la tarjeta y el sitio donde corren.
//
// Dos estados en una sola Activity a propósito: entrar y salir de una app es un
// cambio de estado, no un empujón de pantalla, así el intérprete se abre y se
// cierra en un solo lugar y no queda vivo si alguien navega de otra forma.
//
// Atrás se le pasa a la app: si lo atiende (devuelve true) se sigue adentro, y
// si no, se vuelve al catálogo. Atrás MANTENIDO sale siempre, así una app que
// se coma el botón no puede dejar al usuario encerrado.
//
// Y es el HOST de las puertas (contrato v1): la app encola pedidos
// (`cp.listen`, `cp.call`, `cp.download`, `cp.view`, `cp.open_book`) y esta
// Activity los atiende desde su loop() con sus propias pantallas — escucha,
// conexión, "esperando al servidor", el visor — y le contesta por
// `on_heard` / `on_reply`. El micrófono y la red corren acá, en el loop de
// Arduino, nunca en el worker de Lua (32 KB de stack: el TLS no entra).
// Política de red copiada de Hablar: la radio se levanta la primera vez que
// hace falta y queda arriba hasta que la app se cierra; `WiFi.setSleep(false)`
// sólo mientras hay una petición en el aire.
class LuaAppsActivity final : public Activity {
 public:
  explicit LuaAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LuaApps", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // La grabadora se bombea desde el loop: sin pausa mientras el micrófono está
  // abierto, como en Hablar.
  bool skipLoopDelay() override { return phase == Phase::Listening; }
  // Escuchando, con un pedido en curso o con la red arriba no se duerme solo:
  // una app que baja un librito no puede quedarse a mitad de descarga.
  bool preventAutoSleep() override { return state == RUNNING && (phase != Phase::Idle || wifiActivated); }

 private:
  enum State : uint8_t { LIST, RUNNING, FAILED };
  // Qué está haciendo el host por la app. Idle = la app tiene la pantalla y los
  // botones; el resto son las pantallas del host.
  enum class Phase : uint8_t { Idle, Listening, Connecting, Transcribing, Calling, Downloading, Viewing };

  void startSelected();
  void backToList();
  void renderList();
  void renderError();
  int visibleRows() const;
  void clampScroll();

  // --- Las puertas -----------------------------------------------------------
  void runningLoop();
  // Saca el próximo pedido de la cola de la app y lo arranca. Se llama después
  // de cada callback a la app, que es cuando puede haber encolado algo.
  void pumpRequests();
  void startRequest();
  void beginListen();
  void endListen(bool cancelled);
  // Conexión amigable con las redes guardadas; el selector sólo si ninguna anda.
  void ensureConnected();
  void pumpConnect();
  void onConnected();
  void onConnectFailed();
  // El trabajo síncrono de cada fase, corrido desde loop() DESPUÉS de que la
  // pantalla de la fase se pintó (workPending), como hace Hablar.
  void performTranscribe();
  void performCall();
  void performDownload();
  void openViewer();
  void openBook();
  // Cancela el pedido en curso (Atrás) y lo encolado, y se lo dice a la app.
  void cancelCurrent();
  void finishRequest(bool repaint);
  void shutdownRadio();
  void renderListening();
  void renderWaiting(const char* text);
  // Corta un texto en renglones que entren en `width`, hasta `maxLines`.
  std::vector<std::string> wrapLines(int fontId, const std::string& text, int width, int maxLines) const;

  State state = LIST;
  std::vector<LuaApp::Entry> apps;
  int selected = 0;
  int scroll = 0;
  std::unique_ptr<LuaApp> app;
  unsigned long lastTick = 0;
  ButtonNavigator buttonNavigator;

  Phase phase = Phase::Idle;
  LuaApp::Request current;  // el pedido en curso, ya sacado de la cola
  bool workPending = false;  // la fase pintó su pantalla: en el próximo loop se hace el trabajo
  std::unique_ptr<VoiceRecorder> recorder;  // uno por escucha: los segundos los pide la app
  FriendlyWifi wifi;
  bool wifiActivated = false;
  bool wifiPicker = false;  // la pantalla de selección tiene el foco
  bool requestMade = false;  // hubo TLS: el lector, si se abre, necesita el reinicio silencioso
  unsigned long lastWifiPumpMs = 0;
};
