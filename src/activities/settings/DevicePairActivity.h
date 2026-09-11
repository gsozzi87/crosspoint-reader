#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "activities/home/HubSyncActivity.h"  // FriendlyWifi

// Vincular el aparato con una cuenta de la web, SIN teclado.
//
// El aparato no puede pedir correo ni contraseña, así que el enlace va al
// revés: el aparato pide un código de seis dígitos, lo muestra grande en la
// pantalla, y la persona lo escribe en /board estando ya con su sesión
// iniciada. Mientras tanto la pantalla pregunta cada tres segundos si ya quedó
// vinculado, y cuando pasa muestra con qué cuenta.
//
//   POST /api/pair/start   {deviceId, token}  -> {ok, code, expiresIn}   (sin token)
//   GET  /api/pair/status                     -> {ok, paired, account}   (con token)
//
// El token del aparato se genera solo la primera vez (ServerCredentialStore::
// ensureToken), así que no hay nada que escribir en ningún lado.
class DevicePairActivity final : public Activity {
 public:
  explicit DevicePairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DevicePair", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == WAITING; }

 private:
  enum State { CONNECTING, WAITING, PAIRED, FAILED };

  FriendlyWifi wifi;
  bool wifiPicker = false;

  State state = CONNECTING;
  std::string code;
  std::string account;
  StrId failure = StrId::STR_HUB_SYNC_FAILED;
  std::string failureDetail;
  unsigned long lastPoll = 0;
  unsigned long codeStartedAt = 0;
  int partials = 0;
  bool wifiWasUp = false;

  void beginConnect();
  void pumpConnect();
  void onWifiReady(bool connected);
  void requestCode();
  void poll();
  void fail(StrId why, std::string detail = "");
};
