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

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "NOTES";
constexpr int ROW_H = 60;
constexpr int SIDE = 20;
constexpr int HINT_H = 20;  // línea de ayuda arriba de la barra de botones
constexpr unsigned long MENU_HOLD_MS = 1200;
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
constexpr uint32_t SAVE_TIMEOUT_MS = 30000;
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
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/notes", body, resp, SAVE_TIMEOUT_MS);
  WiFi.setSleep(true);
  int id = 0;
  if (r == ServerClient::Result::Ok) {
    JsonDocument doc;
    if (deserializeJson(doc, resp.body) == DeserializationError::Ok) id = doc["id"] | 0;
  }
  const bool retryable = r == ServerClient::Result::NoNetwork || r == ServerClient::Result::Transport;
  if (retryable) SERVER_CLIENT.enqueue("/api/notes", body);
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
  message(retryable ? StrId::STR_NOTES_QUEUED : StrId::STR_NOTES_SAVE_FAILED,
          retryable ? noteText : std::string(tr(STR_NOTES_KEPT_HERE)));
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

void NotesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_NOTES));
  const int top = metrics.topPadding + metrics.headerHeight + 12;
  // El margen de abajo lleva verticalSpacing además del alto de los hints
  // (misma cuenta que SettingsActivity), o la última fila queda pegada a la
  // barra de botones.
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - HINT_H;
  const char* confirmLabel = "";
  const char* hint = "";

  switch (state) {
    case LIST: {
      const int count = rowCount();
      const int perPage = std::max(1, (bottom - top) / ROW_H);
      if (listTop > index) listTop = index;
      if (index >= listTop + perPage) listTop = index - perPage + 1;
      if (listTop < 0 || listTop >= count) listTop = 0;
      const int width = pageWidth - 2 * SIDE;
      for (int i = listTop; i < count && i < listTop + perPage; ++i) {
        const int y = top + (i - listTop) * ROW_H;
        const Row& row = rows[i];
        if (i == index) drawSelectionRow(renderer, SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4);
        if (row.kind == Row::ADD_TEXT || row.kind == Row::ADD_VOICE) {
          const char* label = row.kind == Row::ADD_TEXT ? tr(STR_NOTES_ADD) : tr(STR_NOTES_ADD_VOICE);
          renderer.drawText(UI_12_FONT_ID, SIDE, y + 16,
                            renderer.truncatedText(UI_12_FONT_ID, label, width, EpdFontFamily::BOLD).c_str(),
                            SELECTION_INK, EpdFontFamily::BOLD);
          continue;
        }
        if (row.kind == Row::VOICE) {
          const voicenotes::Note& note = voiceNotes[row.index];
          const std::string title =
              std::string(tr(STR_NOTES_VOICE_TAG)) + "  ·  " + voicenotes::mmss(note.seconds);
          renderer.drawText(UI_12_FONT_ID, SIDE, y + 6,
                            renderer.truncatedText(UI_12_FONT_ID, title.c_str(), width).c_str(), SELECTION_INK);
          std::string when = voicenotes::when(note);
          if (when.empty() && note.number > 0) when = "#" + std::to_string(note.number);
          if (!when.empty()) {
            renderer.drawText(UI_10_FONT_ID, SIDE, y + 32,
                              renderer.truncatedText(UI_10_FONT_ID, when.c_str(), width).c_str(), SELECTION_INK);
          }
          continue;
        }
        const std::string& text = HUB_STORE.notes[row.index].text;
        const std::string line1 = renderer.truncatedText(UI_12_FONT_ID, text.c_str(), width);
        renderer.drawText(UI_12_FONT_ID, SIDE, y + 6, line1.c_str(), SELECTION_INK);
        // Segunda línea: lo que no entró en la primera (el helper corta con "...").
        if (line1.size() >= 3 && line1.size() < text.size() + 3 &&
            text.compare(0, line1.size() - 3, line1, 0, line1.size() - 3) == 0) {
          const std::string rest = text.substr(line1.size() - 3);
          if (!rest.empty()) {
            renderer.drawText(UI_10_FONT_ID, SIDE, y + 32,
                              renderer.truncatedText(UI_10_FONT_ID, rest.c_str(), width).c_str(), SELECTION_INK);
          }
        }
      }
      if (count <= 2) {
        renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * ROW_H + 40, tr(STR_NOTES_NONE_YET));
      }
      const Row* row = currentRow();
      const bool onAdd = !row || row->kind == Row::ADD_TEXT || row->kind == Row::ADD_VOICE;
      hint = onAdd ? tr(STR_NOTES_ADD_HINT) : tr(STR_NOTES_DELETE_HINT);
      confirmLabel = !row                       ? ""
                     : row->kind == Row::VOICE  ? tr(STR_NOTES_PLAY)
                     : row->kind == Row::TEXT   ? tr(STR_SELECT)
                                                : tr(STR_NOTES_RECORD);
      break;
    }
    case RECORDING: {
      const int seconds = recorder ? static_cast<int>(recorder->seconds()) : 0;
      renderer.drawCenteredText(
          UI_12_FONT_ID, mid - 90,
          renderer
              .truncatedText(UI_12_FONT_ID, take == TAKE_TEXT ? tr(STR_NOTES_REC_TEXT) : tr(STR_NOTES_REC_VOICE),
                             pageWidth - 40, EpdFontFamily::BOLD)
              .c_str(),
          true, EpdFontFamily::BOLD);
      // "0:12 / 1:30": los segundos que van y el tope de esta grabación.
      const std::string counter = std::string(tr(STR_REC_ELAPSED)) + "   " + voicenotes::mmss(seconds) + " / " +
                                  voicenotes::mmss(static_cast<int>(maxSeconds));
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, counter.c_str(), true, EpdFontFamily::BOLD);
      // Barra: lo mismo, de un vistazo.
      const int barW = pageWidth - 2 * SIDE - 40;
      const int barX = (pageWidth - barW) / 2;
      renderer.drawRoundedRect(barX, mid - 4, barW, 16, 2, 4, true);
      const int filled = maxSeconds > 0 ? std::min(barW - 4, static_cast<int>((barW - 4) * seconds / static_cast<int>(maxSeconds))) : 0;
      if (filled > 0) renderer.fillRect(barX + 2, mid - 2, filled, 12);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 40, tr(STR_NOTES_REC_HINT));
      confirmLabel = tr(STR_SELECT);
      break;
    }
    case REVIEW:
    case REVIEW_PLAYING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 70,
                                state == REVIEW ? tr(STR_NOTE_REVIEW_TITLE) : tr(STR_NOTE_REVIEW_PLAYING), true,
                                EpdFontFamily::BOLD);
      const int seconds = recorder ? static_cast<int>(recorder->spokenSeconds() + 0.5f) : 0;
      char len[64];
      snprintf(len, sizeof(len), tr(STR_NOTE_REVIEW_LENGTH), seconds);
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 20, len);
      int hy = mid + 40;
      for (const std::string& line :
           renderer.wrappedText(UI_10_FONT_ID, tr(STR_NOTE_REVIEW_HINT), pageWidth - 60, 6)) {
        renderer.drawCenteredText(UI_10_FONT_ID, hy, line.c_str());
        hy += 26;
      }
      confirmLabel = tr(STR_SELECT);
      break;
    }
    case CONNECTING:
      if (!wifiPicker) FriendlyWifi::drawStatus(renderer, wifi, mid);
      break;
    case SENDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10,
                                sendStep == 0 ? tr(STR_NOTES_TRANSCRIBING) : tr(STR_NOTES_SAVING), true,
                                EpdFontFamily::BOLD);
      break;
    case PLAYING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_NOTES_PLAYING), true, EpdFontFamily::BOLD);
      const Row* row = currentRow();
      if (row && row->kind == Row::VOICE) {
        const voicenotes::Note& note = voiceNotes[row->index];
        std::string line = voicenotes::mmss(note.seconds);
        const std::string when = voicenotes::when(note);
        if (!when.empty()) line += "  ·  " + when;
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 6, line.c_str());
      }
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 40, tr(STR_NOTES_PLAY_STOP));
      break;
    }
    case MESSAGE: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 60,
                                renderer.truncatedText(UI_12_FONT_ID, I18N.get(messageId), pageWidth - 40,
                                                       EpdFontFamily::BOLD)
                                    .c_str(),
                                true, EpdFontFamily::BOLD);
      int y = mid - 20;
      for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, messageDetail.c_str(), pageWidth - 60, 6)) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
        y += 26;
      }
      confirmLabel = tr(STR_SELECT);
      break;
    }
  }

  renderer.drawCenteredText(SMALL_FONT_ID, bottom + 4,
                            renderer.truncatedText(SMALL_FONT_ID, hint, pageWidth - 2 * SIDE).c_str());

  if (state == LIST && confirming && confirm.processRender(renderer, mappedInput)) return;
  const bool navigable = state == LIST;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, navigable ? tr(STR_DIR_UP) : "",
                                            navigable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Regla del panel: refresco limpio cada 10-15 parciales o la pantalla
  // fantasmea. Mientras el micrófono está abierto se evita: un refresco limpio
  // es medio segundo de SPI y ahí se pierden muestras; en su lugar se pide uno
  // al entrar y otro al salir de la grabación (forceClean).
  const bool clean = forceClean || (state != RECORDING && ++partialCount >= PARTIALS_BEFORE_CLEAN);
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
