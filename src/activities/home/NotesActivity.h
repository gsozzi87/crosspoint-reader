#pragma once

#include <memory>
#include <string>
#include <vector>

#include "HubSyncActivity.h"  // FriendlyWifi: conexion sin la pantalla tecnica
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "voice/SpeechOut.h"
#include "voice/VoiceNotes.h"
#include "voice/VoiceRecorder.h"

// Notas. El aparato no tiene teclado, así que hay dos formas de agregar una y
// las dos están en las dos primeras filas, con todas las letras:
//
//   - "Nueva nota (dictada)": graba, POST /api/transcribe y POST /api/notes.
//     NO pasa por /api/voice: ese endpoint corre el clasificador de intención
//     con el LLM y para una nota eso solo agrega la pantalla de "Pensando" y la
//     espera. El tope de grabación es largo (ver TARGET_NOTE_SECONDS) y la
//     pantalla muestra los segundos que van y el tope mientras graba.
//   - "Nueva nota de voz": graba y deja el audio en la tarjeta
//     (/.crosspoint/voicenotes/, `src/voice/VoiceNotes.h`). No sale del aparato:
//     ni se sube ni aparece en GET /api/hub.
//
// En la lista van primero las notas de voz (OK las reproduce) y después las de
// texto (OK las abre paginadas). Atrás mantenido borra la que esté elegida.
class NotesActivity final : public Activity {
 public:
  explicit NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notes", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  // Mientras graba, sube o reproduce no se duerme; en la lista sí.
  // REVIEW espera al usuario sin nada abierto (el micrófono ya cerró): que el
  // aparato se duerma ahí es correcto, y descarta la toma como cualquier otra
  // pantalla que se abandona.
  bool preventAutoSleep() override { return state != LIST && state != MESSAGE && state != REVIEW; }

 private:
  // Lo que se le pide al `VoiceRecorder`. Lo que queda de verdad lo decide
  // `voicenotes::maxRecordSeconds()` con la PSRAM libre del momento (un minuto
  // y medio de PCM son 2,9 MB) y se muestra en pantalla mientras graba.
  static constexpr uint32_t TARGET_NOTE_SECONDS = 90;

  enum State {
    LIST,       // la lista de notas
    RECORDING,  // el micrófono abierto (dictado o nota de voz)
    CONNECTING, // subiendo el WiFi para transcribir
    SENDING,    // transcribiendo y guardando en el servidor
    PLAYING,    // sonando una nota de voz
    REVIEW,         // la nota recién grabada, antes de guardarla
    REVIEW_PLAYING, // escuchando esa nota recién grabada
    MESSAGE,    // un cartel (error o aviso) con OK/Atrás para volver
  };
  enum Take { TAKE_TEXT, TAKE_VOICE };
  // Una fila de la lista.
  struct Row {
    enum Kind { ADD_TEXT, ADD_VOICE, VOICE, TEXT } kind = ADD_TEXT;
    int index = 0;  // dentro de voiceNotes (VOICE) o de HUB_STORE.notes (TEXT)
  };

  State state = LIST;
  Take take = TAKE_TEXT;
  int index = 0;
  int listTop = 0;
  bool forceClean = false;  // entrar o salir de la grabación pide uno limpio
  std::vector<Row> rows;
  std::vector<voicenotes::Note> voiceNotes;

  ButtonNavigator buttonNavigator;
  OptionPopup confirm;
  bool confirming = false;
  std::vector<std::string> confirmOptions;

  // Se crea al empezar a grabar, con los segundos que entren en la memoria
  // libre de ese momento (no es lo mismo recién arrancado que con un libro
  // abierto), y se suelta al terminar.
  std::unique_ptr<VoiceRecorder> recorder;
  uint32_t maxSeconds = TARGET_NOTE_SECONDS;
  int shownSecond = -1;  // último segundo pintado, para no repintar de más
  SpeechOut speech;
  unsigned long playStartedAt = 0;

  FriendlyWifi wifi;
  bool wifiPicker = false;
  bool wifiActivated = false;
  int sendStep = 0;  // 0 transcribir, 1 guardar
  std::string noteText;

  StrId messageId = StrId::STR_NOTES_SAVE_FAILED;
  std::string messageDetail;

  void reloadRows();
  void reloadVoiceNotes();
  int rowCount() const { return static_cast<int>(rows.size()); }
  const Row* currentRow() const;

  void startRecording(Take what);
  void stopRecording();
  void saveVoiceNote();
  void beginConnect();
  void pumpConnect();
  void onWifiSelectionComplete(bool connected);
  void transcribeTake();
  void saveTextNote();

  // Las filas no miden todas lo mismo: las dos de agregar son de un renglón y
  // las notas de dos, así que la ventana se calcula midiendo.
  int rowHeight(int i) const;
  bool needsSectionHeader(int i) const;
  void renderList(int x, int top, int w, int bottom);
  void renderReview(int mid);

  void openCurrent();
  void playCurrent();
  void stopPlaying();
  void deleteCurrent();
  void message(StrId id, std::string detail = "");
  int nextLocalId() const;
};
