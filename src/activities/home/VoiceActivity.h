#pragma once

#include <string>

#include "HubSyncActivity.h"  // FriendlyWifi: conexion sin la pantalla tecnica
#include "activities/Activity.h"
#include "voice/SpeechOut.h"
#include "voice/VoiceRecorder.h"

// The hub's Talk tile (push-to-talk): one take, POST /api/voice, and the
// server decides what it was — a question it answers, a reminder / task /
// shopping item / note / message it saves — then the hub cache is refreshed so
// the widgets show the result, and the reply is shown in the paged viewer.
// Back returns to the hub through a silent restart (WiFi heap).
class VoiceActivity final : public Activity {
 public:
  explicit VoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Voice", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != REPLY && state != FAILED; }

 private:
  // SPEAKING: la respuesta se está reproduciendo y hay algo que hacer después.
  // Hace falta porque el I2S es uno solo: abrir el micrófono mientras habla el
  // parlante falla ("error de micrófono"), y navegar mientras habla corta la frase.
  enum State { RECORDING, CONNECTING, SENDING, SPEAKING, REPLY, FAILED };
  State state = RECORDING;
  // AFTER_AGENDA / AFTER_NOTES: lo que se dicto se ABRE en su pantalla para
  // confirmarlo (y, en un recordatorio, para corregirle hora y repeticion)
  // en vez de quedar guardado a ciegas detras de un cartel que dice "listo".
  enum AfterSpeech { AFTER_NONE, AFTER_ASK_TIME, AFTER_TIMER, AFTER_AGENDA, AFTER_NOTES };
  AfterSpeech afterSpeech = AFTER_NONE;
  // Lo unico que guardo el servidor, si guardo exactamente una cosa: que tipo
  // y con que id, para poder abrirlo. Con varias acciones de un tiron no se
  // abre nada (no hay una pantalla "la correcta") y se muestra el resumen.
  int savedId = 0;
  std::string savedKind;
  unsigned long speakStartedAt = 0;

  VoiceRecorder recorder{20};  // 20 s: con 12 se cortaba a mitad de frase
  std::string heard;   // what the server understood
  std::string intent;  // question | reminder | task | ...
  std::string reply;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  bool wifiActivated = false;
  bool requestPending = false;
  FriendlyWifi wifi;
  bool wifiPicker = false;  // la pantalla de seleccion tiene el foco
  int timerSeconds = 0;  // timer/alarm intent: hand off to TimerActivity
  SpeechOut speech;      // the reply, spoken by the server's Piper, played with the text
  std::string pendingTitle;  // reminder waiting for its hour (the server asked)
  std::string pendingDate;   // día que ya se había dicho ("2026-09-08"), vacío si no dijo ninguno
  bool askingTime = false;

  void startRecording();
  void stopRecording();
  void beginConnect();
  void pumpConnect();
  void onWifiSelectionComplete(bool connected);
  void performRequest();
  void showReply();
  // Espera a que termine de hablar y recién ahí graba de nuevo o abre el reloj.
  void speakThen(AfterSpeech what);
  void runAfterSpeech();
  void fail(StrId why, std::string detail = "");
  void leave();
  const char* intentTitle() const;
};
