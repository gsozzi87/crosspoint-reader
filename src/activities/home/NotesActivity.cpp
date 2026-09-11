#include "NotesActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>
#include <string>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ListStyle.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/SevenSegment.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "NOTES";

constexpr unsigned long MENU_HOLD_MS = 1200;
constexpr uint32_t SAVE_TIMEOUT_MS = 30000;
// Contador de la grabación y de la revisión: los mismos dígitos de segmentos
// del temporizador y del reproductor, que se leen de lejos y no dependen de
// ninguna cara cargada de la tarjeta.
constexpr int DIGIT_W = 34;
constexpr int DIGIT_H = 56;
constexpr int DIGIT_T = 7;
constexpr int DIGIT_GAP = 8;

// Ancho que va a ocupar sevenseg::clock, para centrarlo sin dibujarlo dos veces.
int clockWidth(const int seconds) {
  const int m = (seconds < 0 ? 0 : seconds) / 60;
  const int lead = m >= 10 ? 2 : 1;
  return (lead + 1) * (DIGIT_W + DIGIT_GAP) + DIGIT_W / 2 + DIGIT_GAP + DIGIT_W;
}
}  // namespace

// --- lista ------------------------------------------------------------------

void NotesActivity::reloadVoiceNotes() { voiceNotes = voicenotes::list(); }

void NotesActivity::reloadRows() {
  rows.clear();
  rows.push_back({Row::ADD_TEXT, 0});
  rows.push_back({Row::ADD_VOICE, 0});
  // Las notas de voz primero (son las de acá, las últimas que se grabaron) y
  // después las de texto, que ya vienen ordenadas por el servidor.
  for (int i = 0; i < static_cast<int>(voiceNotes.size()); ++i) rows.push_back({Row::VOICE, i});
  for (int i = 0; i < static_cast<int>(HUB_STORE.notes.size()); ++i) rows.push_back({Row::TEXT, i});
  if (index >= rowCount()) index = rowCount() - 1;
  if (index < 0) index = 0;
}

const NotesActivity::Row* NotesActivity::currentRow() const {
  if (index < 0 || index >= rowCount()) return nullptr;
  return &rows[index];
}

int NotesActivity::nextLocalId() const {
  // Ids negativos para lo que todavía no tiene id del servidor: no chocan con
  // los suyos y la próxima sincronización los reemplaza por los de verdad.
  int lowest = 0;
  for (const HubStore::Note& n : HUB_STORE.notes) lowest = std::min(lowest, n.id);
  return lowest - 1;
}

void NotesActivity::onEnter() {
  Activity::onEnter();
  index = 0;
  listTop = 0;
  reloadVoiceNotes();
  reloadRows();
  if (focusNoteId_ > 0) {
    for (size_t i = 0; i < rows.size(); i++) {
      if (rows[i].kind != Row::TEXT) continue;
      if (rows[i].index < 0 || static_cast<size_t>(rows[i].index) >= HUB_STORE.notes.size()) continue;
      if (HUB_STORE.notes[rows[i].index].id != focusNoteId_) continue;
      index = static_cast<int>(i);
      break;
    }
    focusNoteId_ = 0;  // sólo al entrar
  }
  requestUpdate();
}

void NotesActivity::onExit() {
  Activity::onExit();
  if (recorder) recorder->abort();
  speech.stop();
  if (wifiActivated) {
    // Mismo cierre que las demás pantallas de red: el heap que dejó TLS no se
    // recupera de otra forma.
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void NotesActivity::message(const StrId id, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(id), detail.c_str());
  messageId = id;
  messageDetail = std::move(detail);
  state = MESSAGE;
  forceClean = true;
  requestUpdate();
}

// --- grabar -----------------------------------------------------------------

void NotesActivity::startRecording(const Take what) {
  speech.stop();  // el parlante y el micrófono comparten el I2S
  take = what;
  // El tope real depende de la memoria libre de este momento; la pantalla lo
  // muestra, así que el usuario nunca se queda sin saber cuánto puede hablar.
  maxSeconds = voicenotes::maxRecordSeconds(TARGET_NOTE_SECONDS);
  recorder = std::make_unique<VoiceRecorder>(maxSeconds);
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder->start(why)) {
    recorder.reset();
    message(why);
    return;
  }
  shownSecond = -1;
  forceClean = true;
  state = RECORDING;
  requestUpdate();
}

void NotesActivity::stopRecording() {
  recorder->stop();
  forceClean = true;
  if (recorder->tooShort()) {  // toque sin querer
    recorder->abort();
    recorder.reset();
    state = LIST;
    requestUpdate();
    return;
  }
  if (take == TAKE_VOICE) {
    // La nota de voz se escucha antes de guardarse: sin teclado, lo único que
    // el usuario puede corregir es volver a decirla, y para eso primero tiene
    // que saber qué quedó grabado.
    state = REVIEW;
    requestUpdate();
    return;
  }
  wifiActivated = true;
  beginConnect();
}

// La nota de voz no sale del aparato: se guarda tal cual, en ADPCM, en la SD.
void NotesActivity::saveVoiceNote() {
  const uint8_t* data = recorder->adpcm();
  const size_t len = recorder->adpcmBytes();
  if (!data || len == 0) {
    recorder.reset();
    message(StrId::STR_AUDIO_NO_MEMORY);
    return;
  }
  voicenotes::Note saved;
  const bool ok = voicenotes::save(data, len, saved);
  recorder.reset();  // suelta la PSRAM antes de releer la tarjeta
  if (!ok) {
    message(StrId::STR_NOTES_VOICE_SAVE_FAILED);
    return;
  }
  reloadVoiceNotes();
  reloadRows();
  for (int i = 0; i < rowCount(); ++i) {
    if (rows[i].kind == Row::VOICE && voiceNotes[rows[i].index].path == saved.path) {
      index = i;
      break;
    }
  }
  state = LIST;
  requestUpdate();
}

// --- dictado: WiFi, transcripción y POST /api/notes -------------------------

void NotesActivity::beginConnect() {
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) {
    pumpConnect();
    return;
  }
  requestUpdate();
}

void NotesActivity::pumpConnect() {
  if (wifiPicker) return;
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase phase = wifi.pump();
  if (phase == FriendlyWifi::Phase::Connected) {
    onWifiSelectionComplete(true);
    return;
  }
  if (phase == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             onWifiSelectionComplete(!result.isCancelled);
                           });
    return;
  }
  if (wifi.revision() != rev) requestUpdate();
}

void NotesActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    if (recorder) recorder->abort();
    recorder.reset();
    message(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  sendStep = 0;
  state = SENDING;  // el trabajo va desde loop() para que la pantalla pinte primero
  requestUpdate();
}

void NotesActivity::transcribeTake() {
  if (!SERVER_STORE.hasToken()) {
    recorder.reset();
    message(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  noteText.clear();
  std::string detail;
  const bool ok = SpeechToText::transcribe(*recorder, noteText, detail);
  recorder.reset();
  if (!ok) {
    message(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  sendStep = 1;
  requestUpdate();  // "Guardando la nota..." antes de bloquear en el POST
}

// POST /api/notes: la nota va derecho al store del servidor, sin pasar por el
// clasificador de intención de /api/voice (que es lo que hacía la espera larga
// y la pantalla de "Pensando" para algo que ya sabemos que es una nota).
void NotesActivity::saveTextNote() {
  std::string body;
  {
    JsonDocument doc;
    doc["text"] = noteText;
    doc["lang"] = uiLanguageCode();
    serializeJson(doc, body);
  }
  // postOrQueue y no postJson: la nota tiene que sobrevivir a un servidor
  // saturado (429), caído (5xx) o que todavía no reconoce al aparato (401), no
  // sólo a la falta de red. Antes esos tres devolvían error, la nota NO se
  // encolaba, y la única copia quedaba en la caché del hub — que la próxima
  // sincronización reemplaza con las notas del servidor. O sea: la pantalla
  // decía "guardada" y la nota se perdía sola un rato después.
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postOrQueue("/api/notes", body, &resp, SAVE_TIMEOUT_MS);
  WiFi.setSleep(true);
  int id = 0;
  if (r == ServerClient::Result::Ok) {
    JsonDocument doc;
    if (deserializeJson(doc, resp.body) == DeserializationError::Ok) id = doc["id"] | 0;
  }
  LOG_INF(TAG, "POST /api/notes (%u caracteres): %s", (unsigned)noteText.size(), ServerClient::resultName(r));

  // Pase lo que pase con el servidor, lo transcripto no se pierde: entra en la
  // caché local y se ve en la lista al instante.
  HUB_STORE.notes.insert(HUB_STORE.notes.begin(), HubStore::Note{id != 0 ? id : nextLocalId(), noteText});
  if (static_cast<int>(HUB_STORE.notes.size()) > HubStore::MAX_NOTES) HUB_STORE.notes.resize(HubStore::MAX_NOTES);
  HUB_STORE.saveToFile();
  reloadRows();
  index = 2 + static_cast<int>(voiceNotes.size());  // la nota nueva, arriba de las de texto
  if (index >= rowCount()) index = rowCount() - 1;

  if (r == ServerClient::Result::Ok) {
    state = LIST;
    forceClean = true;
    requestUpdate();
    return;
  }
  // `Queued` es la respuesta de postOrQueue cuando la guardó para más tarde:
  // ese es el caso en que se le puede decir al usuario que va a salir sola.
  const bool enCola = r == ServerClient::Result::Queued;
  message(enCola ? StrId::STR_NOTES_QUEUED : StrId::STR_NOTES_SAVE_FAILED,
          enCola ? noteText : std::string(tr(STR_NOTES_KEPT_HERE)));
}

// --- abrir, reproducir y borrar ---------------------------------------------

void NotesActivity::openCurrent() {
  const Row* row = currentRow();
  if (!row || row->kind != Row::TEXT) return;
  // El visor del diccionario ya pagina texto plano con la palanca: una nota
  // larga se lee ahí sin escribir otro paginador.
  const std::string text = HUB_STORE.notes[row->index].text;
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, tr(STR_HUB_NOTES), text),
                         [this](const ActivityResult&) {
                           forceClean = true;
                           requestUpdate();
                         });
}

void NotesActivity::playCurrent() {
  const Row* row = currentRow();
  if (!row || row->kind != Row::VOICE) return;
  if (!voicenotes::play(voiceNotes[row->index], speech)) {
    message(StrId::STR_NOTES_PLAY_FAILED);
    return;
  }
  playStartedAt = millis();
  state = PLAYING;
  forceClean = true;
  requestUpdate();
}

void NotesActivity::stopPlaying() {
  speech.stop();
  state = LIST;
  forceClean = true;
  requestUpdate();
}

void NotesActivity::deleteCurrent() {
  const Row* row = currentRow();
  if (!row) return;
  if (row->kind == Row::VOICE) {
    voicenotes::remove(voiceNotes[row->index]);
    reloadVoiceNotes();
    reloadRows();
    return;
  }
  if (row->kind != Row::TEXT) return;
  const int id = HUB_STORE.notes[row->index].id;
  HUB_STORE.removeNote(id);
  HUB_STORE.saveToFile();
  if (id > 0) {  // los ids locales (negativos) no existen en el servidor
    std::string body;
    {
      JsonDocument doc;
      doc["kind"] = "note";
      doc["id"] = id;
      doc["action"] = "delete";
      serializeJson(doc, body);
    }
    LOG_INF(TAG, "delete %d: %s", id, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/edit", body)));
  }
  reloadRows();
}

// --- loop -------------------------------------------------------------------

void NotesActivity::loop() {
  switch (state) {
    case LIST: {
      if (confirming) {
        if (confirm.handleInput(mappedInput, [this] { requestUpdate(); })) {
          if (!confirm.isActive()) {
            confirming = false;
            requestUpdate();
          }
        }
        return;
      }
      const int count = rowCount();
      buttonNavigator.onNext([&] {
        if (count > 0) index = ButtonNavigator::nextIndex(index, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) index = ButtonNavigator::previousIndex(index, count);
        requestUpdate();
      });
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
        const Row* row = currentRow();
        if (!row || (row->kind != Row::TEXT && row->kind != Row::VOICE)) return;  // las filas de agregar no se borran
        confirming = true;
        confirmOptions = {tr(STR_AGENDA_DELETE), tr(STR_BACK)};
        confirm.show(StrId::STR_HUB_NOTES, confirmOptions, 1, [this](int idx) {
          if (idx == 0) deleteCurrent();
          confirming = false;
          requestUpdate();
        });
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        const Row* row = currentRow();
        if (!row) return;
        switch (row->kind) {
          case Row::ADD_TEXT: startRecording(TAKE_TEXT); break;
          case Row::ADD_VOICE: startRecording(TAKE_VOICE); break;
          case Row::VOICE: playCurrent(); break;
          case Row::TEXT: openCurrent(); break;
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      break;
    }
    case RECORDING: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder->abort();
        recorder.reset();
        forceClean = true;
        state = LIST;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder->isRecording()) {
        stopRecording();
        break;
      }
      if (!recorder->pump()) {
        recorder.reset();
        message(StrId::STR_AUDIO_CAPTURE_FAILED);
        break;
      }
      // El contador tiene que verse, pero cada repintado es un refresco del
      // papel: al principio y al final va segundo a segundo (que es cuando
      // importa) y en el medio de a cinco, igual que el temporizador.
      const int seconds = static_cast<int>(recorder->seconds());
      const int left = static_cast<int>(maxSeconds) - seconds;
      if (seconds != shownSecond && (seconds <= 10 || left <= 10 || seconds % 5 == 0)) {
        shownSecond = seconds;
        requestUpdate();
      }
      break;
    }
    case REVIEW: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        saveVoiceNote();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        if (recorder) recorder->abort();
        recorder.reset();
        forceClean = true;
        state = LIST;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        if (recorder && recorder->playSpoken()) {
          playStartedAt = millis();
          state = REVIEW_PLAYING;
          requestUpdate();
        }
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        recorder.reset();  // suelta la PSRAM de la toma vieja antes de pedir otra
        startRecording(TAKE_VOICE);
        break;
      }
      break;
    }
    case REVIEW_PLAYING: {
      if (mappedInput.wasAnyPressed()) {
        if (recorder) recorder->stopPlayback();
        state = REVIEW;
        forceClean = true;
        requestUpdate();
        break;
      }
      // 400 ms de gracia: la tarea de audio tarda un toque en arrancar.
      if (millis() - playStartedAt > 400 && (!recorder || !recorder->isPlayingBack())) {
        state = REVIEW;
        requestUpdate();
      }
      break;
    }
    case CONNECTING:
      if (!wifiPicker && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        if (recorder) recorder->abort();
        recorder.reset();
        state = LIST;
        forceClean = true;
        requestUpdate();
        break;
      }
      pumpConnect();
      break;
    case SENDING:
      if (sendStep == 0) {
        transcribeTake();
      } else {
        saveTextNote();
      }
      break;
    case PLAYING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        stopPlaying();
        break;
      }
      // 400 ms de gracia: la tarea de audio tarda un toque en arrancar.
      if (millis() - playStartedAt > 400 && !speech.isPlaying()) stopPlaying();
      break;
    case MESSAGE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        reloadRows();
        state = LIST;
        forceClean = true;
        requestUpdate();
      }
      break;
  }
}

// --- pantalla ---------------------------------------------------------------

int NotesActivity::rowHeight(const int i) const {
  if (i < 0 || i >= rowCount()) return listui::ROW1_H;
  const Row::Kind kind = rows[i].kind;
  // Las dos filas de agregar son de un renglón; una nota lleva su detalle
  // debajo (cuándo se grabó, cuánto dura, si todavía no está transcripta).
  return kind == Row::ADD_TEXT || kind == Row::ADD_VOICE ? listui::ROW1_H : listui::ROW2_H;
}

bool NotesActivity::needsSectionHeader(const int i) const {
  // El encabezado va una sola vez, delante de la primera nota guardada: separa
  // lo que se hace (las dos acciones) de lo que ya está.
  return i == 2 && rowCount() > 2;
}

void NotesActivity::renderList(const int x, const int top, const int w, const int bottom) {
  const int count = rowCount();
  const auto block = [this](const int i) { return rowHeight(i) + (needsSectionHeader(i) ? listui::SECTION_H : 0); };

  // La ventana se calcula MIDIENDO: con filas de altos distintos, dividir el
  // alto disponible por un alto de fila deja la elegida medio tapada.
  if (listTop > index) listTop = index;
  if (listTop < 0 || listTop >= count) listTop = 0;
  while (listTop < count - 1) {
    int y = top;
    int last = listTop;
    for (int i = listTop; i < count; ++i) {
      if (y + block(i) > bottom) break;
      y += block(i);
      last = i;
    }
    if (index <= last) break;
    ++listTop;
  }

  char counter[48];
  snprintf(counter, sizeof(counter), tr(STR_NOTES_COUNT_FORMAT), count - 2);

  int y = top;
  for (int i = listTop; i < count; ++i) {
    if (y + block(i) > bottom) break;
    if (needsSectionHeader(i)) y = listui::sectionHeader(renderer, x, y, w, tr(STR_NOTES_SAVED), counter);
    const Row& row = rows[i];
    const int h = rowHeight(i);
    listui::RowSpec spec;
    spec.selected = i == index;

    std::string title;
    std::string detail;
    std::string meta;
    switch (row.kind) {
      case Row::ADD_TEXT:
        title = tr(STR_NOTES_ADD);
        spec.bold = true;
        break;
      case Row::ADD_VOICE:
        title = tr(STR_NOTES_ADD_VOICE);
        spec.bold = true;
        break;
      case Row::VOICE: {
        const voicenotes::Note& note = voiceNotes[row.index];
        title = tr(STR_NOTES_VOICE_TAG);
        meta = voicenotes::mmss(note.seconds);  // la duración, en su columna de la derecha
        detail = voicenotes::when(note);
        if (detail.empty() && note.number > 0) detail = "#" + std::to_string(note.number);
        // Una nota de voz no tiene texto hasta que alguien la escucha: decirlo
        // acá evita buscarle el contenido que no tiene.
        detail += detail.empty() ? tr(STR_NOTE_NO_TEXT) : std::string(" · ") + tr(STR_NOTE_NO_TEXT);
        break;
      }
      case Row::TEXT: {
        const std::string& text = HUB_STORE.notes[row.index].text;
        title = renderer.truncatedText(UI_12_FONT_ID, text.c_str(), w - 2 * listui::PAD);
        // Segundo renglón: sólo lo que quedó afuera del primero.
        detail = listui::tailAfterEllipsis(title, text);
        break;
      }
    }
    spec.title = title.c_str();
    spec.detail = detail.empty() ? nullptr : detail.c_str();
    spec.meta = meta.empty() ? nullptr : meta.c_str();
    listui::row(renderer, x, y, w, h, spec);
    y += h;
  }

  if (count <= 2) renderer.drawCenteredText(UI_10_FONT_ID, y + 2 * listui::GAP, tr(STR_NOTES_NONE_YET));
}

void NotesActivity::renderReview(const int mid) {
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  const char* title = state == REVIEW ? tr(STR_NOTE_REVIEW_TITLE) : tr(STR_NOTE_REVIEW_PLAYING);
  renderer.drawCenteredText(UI_14_FONT_ID, mid - DIGIT_H - 4 * listui::GAP,
                            renderer.truncatedText(UI_14_FONT_ID, title, w).c_str());

  // Cuánto se grabó, con los mismos dígitos del temporizador.
  const int seconds = recorder ? static_cast<int>(recorder->spokenSeconds() + 0.5f) : 0;
  const int cx = x + (w - clockWidth(seconds)) / 2;
  sevenseg::clock(renderer, seconds, cx, mid - DIGIT_H / 2, DIGIT_W, DIGIT_H, DIGIT_T, DIGIT_GAP);

  int hy = mid + DIGIT_H / 2 + 4 * listui::GAP;
  const int step = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_NOTE_REVIEW_HINT), w, 3)) {
    renderer.drawCenteredText(UI_10_FONT_ID, hy, line.c_str());
    hy += step;
  }
}

void NotesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_NOTES));
  const int top = listui::contentTop();
  const int bottom = listui::contentBottom(renderer);
  const int hintY = bottom - listui::HINT_H;
  const char* confirmLabel = "";
  const char* hintText = "";

  switch (state) {
    case LIST: {
      renderList(x, top, w, hintY - listui::GAP);
      const Row* row = currentRow();
      const bool onAdd = !row || row->kind == Row::ADD_TEXT || row->kind == Row::ADD_VOICE;
      hintText = onAdd ? tr(STR_NOTES_ADD_HINT) : tr(STR_NOTES_DELETE_HINT);
      confirmLabel = !row                      ? ""
                     : row->kind == Row::VOICE ? tr(STR_NOTES_PLAY)
                     : row->kind == Row::TEXT  ? tr(STR_SELECT)
                                               : tr(STR_NOTES_RECORD);
      break;
    }
    case RECORDING: {
      const int seconds = recorder ? static_cast<int>(recorder->seconds()) : 0;
      renderer.drawCenteredText(
          UI_14_FONT_ID, mid - DIGIT_H - 4 * listui::GAP,
          renderer
              .truncatedText(UI_14_FONT_ID, take == TAKE_TEXT ? tr(STR_NOTES_REC_TEXT) : tr(STR_NOTES_REC_VOICE), w)
              .c_str());
      const int cx = x + (w - clockWidth(seconds)) / 2;
      sevenseg::clock(renderer, seconds, cx, mid - DIGIT_H / 2, DIGIT_W, DIGIT_H, DIGIT_T, DIGIT_GAP);
      // Carril de 1 px con el tramo hecho en 3: una barra maciza de 16 px de
      // alto es el manchón que fantasmea en el parcial siguiente.
      const int railY = mid + DIGIT_H / 2 + 3 * listui::GAP;
      renderer.fillRect(x, railY, w, 1, true);
      const int done = maxSeconds > 0 ? std::min(w, static_cast<int>(w * seconds / static_cast<int>(maxSeconds))) : 0;
      if (done > 0) renderer.fillRect(x, railY - 1, done, 3, true);
      // "0:12 / 1:30": los segundos que van, a la izquierda, y el tope de esta
      // grabación a la derecha, en la misma línea de base.
      const int labelY = railY + 2 * listui::GAP;
      renderer.drawText(UI_10_FONT_ID, x, labelY, tr(STR_REC_ELAPSED));
      const std::string cap = voicenotes::mmss(static_cast<int>(maxSeconds));
      renderer.drawText(UI_10_FONT_ID, x + w - renderer.getTextWidth(UI_10_FONT_ID, cap.c_str()), labelY, cap.c_str());
      hintText = tr(STR_NOTES_REC_HINT);
      confirmLabel = tr(STR_SELECT);
      break;
    }
    case REVIEW:
    case REVIEW_PLAYING:
      renderReview(mid);
      confirmLabel = tr(STR_SELECT);
      break;
    case CONNECTING:
      if (!wifiPicker) FriendlyWifi::drawStatus(renderer, wifi, mid);
      break;
    case SENDING:
      renderer.drawCenteredText(UI_14_FONT_ID, mid - 10,
                                sendStep == 0 ? tr(STR_NOTES_TRANSCRIBING) : tr(STR_NOTES_SAVING));
      break;
    case PLAYING: {
      renderer.drawCenteredText(UI_14_FONT_ID, mid - 30, tr(STR_NOTES_PLAYING));
      const Row* row = currentRow();
      if (row && row->kind == Row::VOICE) {
        const voicenotes::Note& note = voiceNotes[row->index];
        std::string line = voicenotes::mmss(note.seconds);
        const std::string when = voicenotes::when(note);
        if (!when.empty()) line += " · " + when;
        renderer.drawCenteredText(UI_10_FONT_ID, mid + listui::GAP, line.c_str());
      }
      hintText = tr(STR_NOTES_PLAY_STOP);
      break;
    }
    case MESSAGE: {
      renderer.drawCenteredText(UI_14_FONT_ID, mid - 60,
                                renderer.truncatedText(UI_14_FONT_ID, I18N.get(messageId), w).c_str());
      int y = mid - 20;
      const int step = renderer.getLineHeight(UI_10_FONT_ID) + 4;
      for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, messageDetail.c_str(), w, 6)) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
        y += step;
      }
      confirmLabel = tr(STR_SELECT);
      break;
    }
  }

  listui::hint(renderer, hintY, hintText);

  if (state == LIST && confirming && confirm.processRender(renderer, mappedInput)) return;
  const bool navigable = state == LIST;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, navigable ? tr(STR_DIR_UP) : "",
                                            navigable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de refrescos limpios la lleva el coordinador del panel; acá
  // sólo se pide uno al entrar y al salir de la grabación (forceClean), porque
  // con el micrófono abierto medio segundo de SPI se come muestras.
  const bool clean = forceClean;
  forceClean = false;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
