#include "VoiceActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>

#include "HubStore.h"
#include "HubSyncActivity.h"
#include "TimerActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "VOICE_ACT";
constexpr uint32_t VOICE_TIMEOUT_MS = 90000;  // Whisper + Claude on one request
constexpr unsigned long SPEAK_MAX_MS = 15000;  // tope por si el audio no termina nunca
constexpr int SIDE = 16;

// Todo lo que el servidor sabe clasificar (server/src/voice.ts: question,
// reminder, task, shopping, note, message, timer, alarm, translate, memory),
// con un ejemplo por tipo. Sin esto el usuario no tiene forma de saber que se
// le puede pedir: la pantalla de grabacion es el unico lugar donde mirarlo.
struct VoiceExample {
  StrId category;
  StrId phrase;
};
const VoiceExample EXAMPLES[] = {
    {StrId::STR_VOICE_CAT_ASK, StrId::STR_VOICE_SAY_ASK},
    {StrId::STR_VOICE_CAT_REMINDER, StrId::STR_VOICE_SAY_REMINDER},
    {StrId::STR_VOICE_CAT_REPEAT, StrId::STR_VOICE_SAY_REPEAT},
    {StrId::STR_VOICE_CAT_TASK, StrId::STR_VOICE_SAY_TASK},
    {StrId::STR_VOICE_CAT_SHOPPING, StrId::STR_VOICE_SAY_SHOPPING},
    {StrId::STR_VOICE_CAT_NOTE, StrId::STR_VOICE_SAY_NOTE},
    {StrId::STR_VOICE_CAT_MESSAGE, StrId::STR_VOICE_SAY_MESSAGE},
    {StrId::STR_VOICE_CAT_TIMER, StrId::STR_VOICE_SAY_TIMER},
    {StrId::STR_VOICE_CAT_ALARM, StrId::STR_VOICE_SAY_ALARM},
    {StrId::STR_VOICE_CAT_TRANSLATE, StrId::STR_VOICE_SAY_TRANSLATE},
    {StrId::STR_VOICE_CAT_MEMORY, StrId::STR_VOICE_SAY_MEMORY},
};
constexpr int EXAMPLE_COUNT = sizeof(EXAMPLES) / sizeof(EXAMPLES[0]);
}  // namespace

void VoiceActivity::onEnter() {
  Activity::onEnter();
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  startRecording();
}

void VoiceActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  speech.stop();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    if (timerSeconds > 0) return;  // TimerActivity takes over; a restart would kill it
    silentRestart();
  }
}

void VoiceActivity::leave() { activityManager.goHome(); }

void VoiceActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

void VoiceActivity::startRecording() {
  speech.stop();  // el parlante y el micrófono comparten el I2S: si sigue hablando, la captura falla
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  state = RECORDING;
  requestUpdate();
}

void VoiceActivity::stopRecording() {
  recorder.stop();
  if (recorder.tooShort()) {
    leave();  // accidental press
    return;
  }
  wifiActivated = true;
  beginConnect();
}

// Conexion amigable: se prueban las redes guardadas mostrando un cartel propio;
// la pantalla de seleccion aparece solo si ninguna anda.
void VoiceActivity::beginConnect() {
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) {  // ya conectado o sin redes guardadas: sin cartel de mas
    pumpConnect();
    return;
  }
  requestUpdate();
}

void VoiceActivity::pumpConnect() {
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

void VoiceActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = SENDING;  // the request runs from loop() so the screen paints first
  requestPending = true;
  requestUpdate();
}

// POST /api/voice (audio/adpcm) -> [u32 json length][json][speech]
void VoiceActivity::performRequest() {
  requestPending = false;
  WiFi.setSleep(false);
  const uint8_t* body = recorder.adpcm();
  const size_t bytes = recorder.adpcmBytes();
  // adpcm() devuelve nullptr si no pudo reservar la PSRAM: sin esto se posteaba
  // un puntero nulo con el tamaño calculado.
  if (!body || bytes == 0) {
    WiFi.setSleep(true);
    recorder.release();
    fail(StrId::STR_AUDIO_NO_MEMORY);
    return;
  }
  LOG_DBG(TAG, "POST /api/voice: %u bytes", (unsigned)bytes);
  std::string path = std::string("/api/voice?lang=") + uiLanguageCode() + "&speak=" + HUB_STORE.speakParam();
  if (!pendingTitle.empty()) path += "&pending=" + urlEncode(pendingTitle);
  if (!pendingDate.empty()) path += "&pendingDate=" + urlEncode(pendingDate);
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postBytes(path, "audio/adpcm", body, bytes, resp, VOICE_TIMEOUT_MS);
  recorder.release();
  if (r != ServerClient::Result::Ok) {
    WiFi.setSleep(true);
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_ASK_FAILED, detail);
    return;
  }
  // Framed body: [u32 LE json length][json][ADPCM speech, optional].
  const std::string& raw = resp.body;
  size_t jsonLen = 0;
  if (raw.size() >= 4) {
    jsonLen = static_cast<uint8_t>(raw[0]) | (static_cast<uint8_t>(raw[1]) << 8) |
              (static_cast<uint8_t>(raw[2]) << 16) | (static_cast<size_t>(static_cast<uint8_t>(raw[3])) << 24);
  }
  const bool framed = jsonLen > 0 && 4 + jsonLen <= raw.size();
  JsonDocument doc;
  const DeserializationError jsonErr =
      framed ? deserializeJson(doc, raw.data() + 4, jsonLen) : deserializeJson(doc, raw);
  if (jsonErr != DeserializationError::Ok) {
    WiFi.setSleep(true);
    fail(StrId::STR_ASK_FAILED, tr(STR_VOICE_BAD_REPLY));
    return;
  }
  heard = doc["text"] | "";
  intent = doc["intent"] | "";
  reply = doc["reply"] | "";
  timerSeconds = doc["timerSeconds"] | 0;
  const char* askTime = doc["askTime"] | "";  // reminder with no time: ask for it
  const size_t audioBytes = framed ? raw.size() - 4 - jsonLen : 0;
  if (audioBytes > 8) {
    // Start the voice right away, while the widgets refresh and the text paints.
    speech.playAdpcm(reinterpret_cast<const uint8_t*>(raw.data() + 4 + jsonLen), audioBytes);
  }
  if (reply.empty()) {
    WiFi.setSleep(true);
    fail(StrId::STR_ASK_FAILED, doc["error"] | tr(STR_VOICE_EMPTY_REPLY));
    return;
  }
  if (askTime[0]) {
    // "Remind me to buy milk tomorrow" with no hour: ask for it and listen
    // again, carrying what is pending so the server keeps title and day.
    // Primero termina de preguntar en voz alta: el micrófono y el parlante
    // comparten el I2S, abrir la captura mientras suena da error de micrófono.
    pendingTitle = askTime;
    pendingDate = doc["askDate"] | "";
    askingTime = true;
    WiFi.setSleep(true);
    speakThen(AFTER_ASK_TIME);
    return;
  }
  pendingTitle.clear();
  pendingDate.clear();
  askingTime = false;
  LOG_INF(TAG, "\"%s\" -> %s (stt=%d llm=%d tts=%d total=%d ms)", heard.c_str(), intent.c_str(),
          (int)(doc["ms"]["stt"] | 0), (int)(doc["ms"]["llm"] | 0), (int)(doc["ms"]["tts"] | 0),
          (int)(doc["ms"]["total"] | 0));

  // Widgets: whatever was just saved shows up on the hub right away.
  if (intent != "question" && intent != "translate" && intent != "memory") HubSyncActivity::fetchNow();
  WiFi.setSleep(true);
  if ((intent == "timer" || intent == "alarm") && timerSeconds > 0) {
    LOG_INF(TAG, "timer %d s", timerSeconds);
    speakThen(AFTER_TIMER);  // que termine de decir "listo, 20 segundos" antes de irse
    return;
  }
  timerSeconds = 0;
  showReply();
}

const char* VoiceActivity::intentTitle() const {
  if (intent == "reminder") return tr(STR_VOICE_SAVED_REMINDER);
  if (intent == "task") return tr(STR_VOICE_SAVED_TASK);
  if (intent == "shopping") return tr(STR_VOICE_SAVED_SHOPPING);
  if (intent == "note") return tr(STR_VOICE_SAVED_NOTE);
  if (intent == "message") return tr(STR_VOICE_SAVED_MESSAGE);
  if (intent == "translate") return tr(STR_VOICE_TRANSLATION);
  if (intent == "memory") return tr(STR_VOICE_SAVED_MEMORY);
  if (intent == "alarm") return tr(STR_VOICE_SAVED_REMINDER);
  return heard.c_str();  // question: what was asked, as the header
}

// La respuesta ya se está reproduciendo (playAdpcm): se espera a que termine y
// recién ahí se hace lo que sigue. Sin audio, sigue de una.
void VoiceActivity::speakThen(const AfterSpeech what) {
  afterSpeech = what;
  if (!speech.hasStarted()) {
    runAfterSpeech();
    return;
  }
  speakStartedAt = millis();
  state = SPEAKING;
  requestUpdate();
}

void VoiceActivity::runAfterSpeech() {
  const AfterSpeech what = afterSpeech;
  afterSpeech = AFTER_NONE;
  speech.stop();  // libera el I2S y la PSRAM antes de grabar o de irse
  if (what == AFTER_ASK_TIME) {
    startRecording();
    return;
  }
  if (what == AFTER_TIMER) {
    activityManager.replaceActivity(std::make_unique<TimerActivity>(renderer, mappedInput, timerSeconds));
    return;
  }
  showReply();
}

void VoiceActivity::showReply() {
  state = REPLY;
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, intentTitle(), reply),
                         [this](const ActivityResult&) { leave(); });
}

void VoiceActivity::loop() {
  switch (state) {
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        leave();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        stopRecording();
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case SENDING:
      if (requestPending) performRequest();
      break;
    case SPEAKING: {
      // 400 ms de gracia: la tarea de audio tarda un toque en arrancar y
      // isPlaying() sería false justo después de pedir la reproducción.
      const unsigned long spoken = millis() - speakStartedAt;
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || spoken > SPEAK_MAX_MS ||
          (spoken > 400 && !speech.isPlaying())) {
        runAfterSpeech();
      }
      break;
    }
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        leave();
      }
      break;
    case CONNECTING:
      if (!wifiPicker && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        leave();
        break;
      }
      pumpConnect();
      break;
    case REPLY:
      break;
  }
}

void VoiceActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_TALK));
  const char* confirmLabel = "";
  switch (state) {
    case RECORDING:
      if (askingTime) {
        renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, tr(STR_VOICE_ASK_TIME), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, mid - 4,
                                  renderer.truncatedText(UI_10_FONT_ID, pendingTitle.c_str(), pageWidth - 40).c_str());
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 26, tr(STR_VOICE_ASK_TIME_HINT));
      } else {
        // Ejemplos de lo que entiende, agrupados por tipo: sin esto hay que
        // adivinar que se le puede pedir. Todo pasa por truncatedText contra el
        // ancho real; un texto centrado mas ancho que la pantalla es lo que
        // llena el log de "[GFX] !! Outside range".
        const int top = metrics.topPadding + metrics.headerHeight + 10;
        renderer.drawCenteredText(
            UI_12_FONT_ID, top,
            renderer.truncatedText(UI_12_FONT_ID, tr(STR_VOICE_PROMPT), pageWidth - 30, EpdFontFamily::BOLD).c_str(),
            true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(
            SMALL_FONT_ID, top + 30,
            renderer.truncatedText(SMALL_FONT_ID, tr(STR_VOICE_EXAMPLES_TITLE), pageWidth - 30).c_str());
        const int listTop = top + 58;
        const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
        const int rowH = std::max(22, std::min(44, (listBottom - listTop) / EXAMPLE_COUNT));
        // La columna de las etiquetas es la mas ancha de todas, con tope: asi
        // el ejemplo siempre tiene lugar aunque el idioma use palabras largas.
        int labelW = 0;
        for (const VoiceExample& ex : EXAMPLES) {
          labelW = std::max(labelW, renderer.getTextWidth(SMALL_FONT_ID, I18N.get(ex.category), EpdFontFamily::BOLD));
        }
        labelW = std::min(labelW, pageWidth / 3);
        const int phraseX = SIDE + labelW + 10;
        const int phraseW = pageWidth - SIDE - phraseX;
        int y = listTop;
        for (const VoiceExample& ex : EXAMPLES) {
          renderer.drawText(
              SMALL_FONT_ID, SIDE, y + 4,
              renderer.truncatedText(SMALL_FONT_ID, I18N.get(ex.category), labelW, EpdFontFamily::BOLD).c_str(), true,
              EpdFontFamily::BOLD);
          renderer.drawText(UI_10_FONT_ID, phraseX, y,
                            renderer.truncatedText(UI_10_FONT_ID, I18N.get(ex.phrase), phraseW).c_str());
          y += rowH;
        }
      }
      confirmLabel = tr(STR_SELECT);
      break;
    case SENDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_VOICE_THINKING), true, EpdFontFamily::BOLD);
      break;
    case SPEAKING:
      // Mientras habla se muestra lo mismo que va a decir; si lo que sigue es
      // preguntar la hora, ya se ve el pedido para no perder tiempo después.
      if (afterSpeech == AFTER_ASK_TIME) {
        renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, tr(STR_VOICE_ASK_TIME), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, mid - 4,
                                  renderer.truncatedText(UI_10_FONT_ID, pendingTitle.c_str(), pageWidth - 40).c_str());
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 26, tr(STR_VOICE_ASK_TIME_HINT));
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, mid - 20,
                                  renderer.truncatedText(UI_12_FONT_ID, reply.c_str(), pageWidth - 40, EpdFontFamily::BOLD).c_str(),
                                  true, EpdFontFamily::BOLD);
      }
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 10,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
      if (!wifiPicker) FriendlyWifi::drawStatus(renderer, wifi, mid);
      break;
    case REPLY:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
