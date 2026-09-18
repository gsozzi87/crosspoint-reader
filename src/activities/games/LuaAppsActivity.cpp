#include "LuaAppsActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "SilentRestart.h"
#include "activities/ListStyle.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "LUAHOST";
constexpr int SIDE = 24;  // ÚNICO margen lateral de la pantalla
// Atrás mantenido sale siempre, aunque la app se coma el botón.
constexpr unsigned long EXIT_HOLD_MS = 1000;
// El servidor contesta un servicio síncrono en menos de 25 s por contrato; lo
// que tarde más es un trabajo que se consulta con `job.status`.
constexpr uint32_t CALL_TIMEOUT_MS = 40000;

size_t fileSize(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return 0;
  const size_t size = f.size();
  f.close();
  return size;
}
}  // namespace

void LuaAppsActivity::onEnter() {
  Activity::onEnter();
  apps = LuaApp::installed();
  selected = 0;
  scroll = 0;
  state = LIST;
  phase = Phase::Idle;
  requestUpdate();
}

void LuaAppsActivity::onExit() {
  // El intérprete se cierra acá pase lo que pase: es lo que garantiza que no
  // quede una app viva si se sale por un camino raro (una alarma, el hub).
  if (recorder) recorder->abort();
  recorder.reset();
  app.reset();
  shutdownRadio();
  Activity::onExit();
}

// La radio se apaga sin reinicio silencioso: la pantalla de abajo (el hub o el
// catálogo) sigue viva y no necesita el heap entero. El único que lo necesita
// es el lector, y ése tiene su propio camino en openBook().
void LuaAppsActivity::shutdownRadio() {
  if (!wifiActivated) return;
  wifiActivated = false;
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

int LuaAppsActivity::visibleRows() const {
  return std::max(1, (listui::contentBottom(renderer) - listui::contentTop()) / listui::ROW2_H);
}

void LuaAppsActivity::clampScroll() {
  const int count = static_cast<int>(apps.size());
  const int rows = visibleRows();
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + rows) scroll = selected - rows + 1;
  scroll = std::max(0, std::min(scroll, std::max(0, count - rows)));
}

void LuaAppsActivity::startSelected() {
  if (apps.empty()) return;
  app = makeUniqueNoThrow<LuaApp>();
  if (!app) {
    state = FAILED;
    requestUpdate();
    return;
  }
  phase = Phase::Idle;
  workPending = false;
  requestMade = false;
  if (!app->open(renderer, apps[selected].path)) {
    state = FAILED;
  } else {
    state = RUNNING;
    lastTick = millis();
    // on_open ya corrió: puede haber pedido algo (un librito arranca escuchando).
    pumpRequests();
  }
  requestUpdate();
}

void LuaAppsActivity::backToList() {
  if (recorder) recorder->abort();
  recorder.reset();
  app.reset();
  phase = Phase::Idle;
  workPending = false;
  // La app se cerró: la red era de ella.
  shutdownRadio();
  state = LIST;
  requestUpdate();
}

void LuaAppsActivity::loop() {
  if (state == LIST) {
    buttonNavigator.onNext([this] {
      if (apps.empty()) return;
      selected = ButtonNavigator::nextIndex(selected, static_cast<int>(apps.size()));
      clampScroll();
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      if (apps.empty()) return;
      selected = ButtonNavigator::previousIndex(selected, static_cast<int>(apps.size()));
      clampScroll();
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      backToList();
    }
    return;
  }

  runningLoop();
}

// --- Corriendo ---------------------------------------------------------------

void LuaAppsActivity::runningLoop() {
  if (!app || !app->ok()) {
    if (recorder) recorder->abort();
    recorder.reset();
    phase = Phase::Idle;
    state = FAILED;
    requestUpdate();
    return;
  }

  // La salida de emergencia va PRIMERO: si la app se cuelga con el botón, esto
  // sigue funcionando. También con el micrófono abierto o esperando la red.
  if (!wifiPicker && mappedInput.wasLongPressed(MappedInputManager::Button::Back, EXIT_HOLD_MS)) {
    backToList();
    return;
  }

  // --- Las fases del host: la app no tiene los botones, salvo Atrás ----------
  switch (phase) {
    case Phase::Listening: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        endListen(/*cancelled=*/true);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder->isRecording()) {
        // La suelta de ese mismo OK llega un cuadro después; si la toma fue
        // demasiado corta la app ya está en Idle y la leería como un "ok" que
        // nadie dio (y en Librito eso es "Seguir" sin mirar lo entendido).
        mappedInput.absorbHeldButton(MappedInputManager::Button::Confirm);
        endListen(/*cancelled=*/false);
        return;
      }
      if (!recorder->pump()) {
        LOG_ERR(TAG, "%s: falló la captura del micrófono", app->name().c_str());
        endListen(/*cancelled=*/true);
        return;
      }
      // El WiFi se conecta mientras se habla, como en Hablar: cada 100 ms se le
      // da cuerda al intento; el selector espera a que termine la toma.
      if (wifi.phase() != FriendlyWifi::Phase::Idle && !wifi.isDone() && millis() - lastWifiPumpMs >= 100) {
        lastWifiPumpMs = millis();
        wifi.pump();
      }
      // on_tick NO corre con el micrófono abierto: cada tick es una tarea nueva
      // de 32 KB al lado del DMA de captura, y la toma no es el momento.
      return;
    }
    case Phase::Connecting:
      if (!wifiPicker && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        cancelCurrent();
        return;
      }
      pumpConnect();
      break;  // on_tick sigue mientras se espera la red
    case Phase::Transcribing:
    case Phase::Calling:
    case Phase::Downloading:
      // Lo que todavía no salió se puede cancelar; lo que está en el aire es un
      // POST síncrono y no (los botones no se leen mientras dura).
      if (workPending && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        cancelCurrent();
        return;
      }
      if (workPending) {
        workPending = false;
        if (phase == Phase::Transcribing) {
          performTranscribe();
        } else if (phase == Phase::Calling) {
          performCall();
        } else {
          performDownload();
        }
      }
      return;
    case Phase::Viewing:
      return;  // el visor tiene el foco
    case Phase::Idle:
      break;
  }

  const char* key = nullptr;
  if (phase == Phase::Idle) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up))
      key = "up";
    else if (mappedInput.wasPressed(MappedInputManager::Button::Down))
      key = "down";
    else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm))
      key = "ok";
    else if (mappedInput.wasReleased(MappedInputManager::Button::Back))
      key = "back";
  }

  if (key) {
    const bool repaint = app->onKey(key);
    // Atrás que la app no atendió = salir. Es lo que evita que una app sin
    // `on_key` deje al usuario adentro sin saber cómo volver. Si en cambio
    // encoló algo (pidió escuchar, por ejemplo), lo atendió.
    if (strcmp(key, "back") == 0 && !repaint && !app->quitRequested() && !app->hasRequests() && app->ok()) {
      backToList();
      return;
    }
    if (repaint) requestUpdate();
  } else if (millis() - lastTick >= LuaApp::TICK_MS) {
    lastTick = millis();
    // Con una fase del host en pantalla el repintado que pida on_tick no se
    // ve: la pantalla es la del host y no cambió.
    if (app->onTick() && phase == Phase::Idle) requestUpdate();
  }

  if (app->quitRequested()) {
    backToList();
    return;
  }
  if (!app->ok()) {
    if (recorder) recorder->abort();
    recorder.reset();
    phase = Phase::Idle;
    state = FAILED;
    requestUpdate();
    return;
  }
  pumpRequests();
}

// --- Las puertas ---------------------------------------------------------------

void LuaAppsActivity::pumpRequests() {
  if (phase != Phase::Idle || !app || !app->ok()) return;
  if (!app->takeRequest(current)) return;
  app->setBusy(true);
  startRequest();
}

void LuaAppsActivity::startRequest() {
  switch (current.kind) {
    case LuaApp::Request::Kind::Listen: beginListen(); return;
    case LuaApp::Request::Kind::Call:
    case LuaApp::Request::Kind::Download:
      // Sin token no hay a quién llamar: se contesta sin levantar la red.
      if (!SERVER_STORE.hasToken()) {
        LOG_ERR(TAG, "%s: pedido %d sin vincular", app->name().c_str(), current.id);
        finishRequest(app->onReplyError(current.id, "sin vincular"));
        return;
      }
      ensureConnected();
      return;
    case LuaApp::Request::Kind::View: openViewer(); return;
    case LuaApp::Request::Kind::OpenBook: openBook(); return;
  }
}

// El pedido terminó (bien, mal o cancelado): la app vuelve a tener la pantalla,
// se repinta (el contrato lo promete) y se mira si encoló otro.
void LuaAppsActivity::finishRequest(const bool repaint) {
  (void)repaint;
  phase = Phase::Idle;
  workPending = false;
  if (app) app->setBusy(false);
  requestUpdate();
  pumpRequests();
}

void LuaAppsActivity::cancelCurrent() {
  bool repaint = false;
  switch (current.kind) {
    case LuaApp::Request::Kind::Listen: repaint = app->onHeard(nullptr); break;
    case LuaApp::Request::Kind::Call:
    case LuaApp::Request::Kind::Download: repaint = app->onReplyError(current.id, "cancelado"); break;
    default: break;
  }
  recorder.reset();
  if (app && app->ok()) repaint |= app->cancelQueued();
  LOG_INF(TAG, "%s: cancelado por Atrás", app ? app->name().c_str() : "?");
  finishRequest(repaint);
}

// --- Escuchar ----------------------------------------------------------------

void LuaAppsActivity::beginListen() {
  // Sin token la transcripción no tiene a dónde ir; se contesta nil sin abrir
  // el micrófono, que es lo que la app entiende como "no se pudo".
  if (!SERVER_STORE.hasToken()) {
    LOG_ERR(TAG, "%s: cp.listen sin vincular", app->name().c_str());
    finishRequest(app->onHeard(nullptr));
    return;
  }
  // La radio se levanta ANTES de abrir el micrófono y se conecta mientras se
  // habla (copiado de Hablar en 1.5.103). Si ya está arriba, no hace nada.
  if (wifi.phase() == FriendlyWifi::Phase::Idle || (!wifi.isDone() && wifi.phase() != FriendlyWifi::Phase::Idle)) {
    if (wifi.phase() == FriendlyWifi::Phase::Idle) wifi.begin();
    wifiActivated = true;
  }
  // No hay nada del aparato hablando acá (la app no reproduce voz), pero el
  // I2S es uno solo igual: la grabadora se abre con el códec libre.
  recorder = makeUniqueNoThrow<VoiceRecorder>(static_cast<uint32_t>(current.seconds));
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder || !recorder->start(why)) {
    LOG_ERR(TAG, "%s: no se pudo abrir el micrófono: %s", app->name().c_str(), I18N.get(why));
    recorder.reset();
    finishRequest(app->onHeard(nullptr));
    return;
  }
  phase = Phase::Listening;
  requestUpdate();
}

void LuaAppsActivity::endListen(const bool cancelled) {
  if (cancelled) {
    recorder->abort();
    cancelCurrent();
    return;
  }
  recorder->stop();
  if (recorder->tooShort()) {
    // Toque accidental: no vale un viaje al servidor.
    recorder.reset();
    finishRequest(app->onHeard(nullptr));
    return;
  }
  ensureConnected();
}

// --- La red ------------------------------------------------------------------

void LuaAppsActivity::ensureConnected() {
  wifiPicker = false;
  if (wifi.phase() == FriendlyWifi::Phase::Idle) wifi.begin();
  wifiActivated = true;
  phase = Phase::Connecting;
  if (wifi.isDone()) {  // ya conectado o sin redes guardadas: sin cartel de más
    pumpConnect();
    return;
  }
  requestUpdate();
}

void LuaAppsActivity::pumpConnect() {
  if (wifiPicker) return;
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase p = wifi.pump();
  if (p == FriendlyWifi::Phase::Connected) {
    onConnected();
    return;
  }
  if (p == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             if (result.isCancelled) {
                               onConnectFailed();
                             } else {
                               onConnected();
                             }
                           });
    return;
  }
  if (wifi.revision() != rev) requestUpdate();
}

void LuaAppsActivity::onConnected() {
  switch (current.kind) {
    case LuaApp::Request::Kind::Listen: phase = Phase::Transcribing; break;
    case LuaApp::Request::Kind::Call: phase = Phase::Calling; break;
    case LuaApp::Request::Kind::Download: phase = Phase::Downloading; break;
    default: finishRequest(false); return;
  }
  // La petición corre desde loop() para que la pantalla de espera se pinte antes.
  workPending = true;
  requestUpdate();
}

void LuaAppsActivity::onConnectFailed() {
  LOG_ERR(TAG, "%s: sin WiFi para el pedido", app ? app->name().c_str() : "?");
  bool repaint = false;
  switch (current.kind) {
    case LuaApp::Request::Kind::Listen: repaint = app->onHeard(nullptr); break;
    case LuaApp::Request::Kind::Call:
    case LuaApp::Request::Kind::Download:
      repaint = app->onReplyError(current.id, "sin conexión con el servidor (wifi)");
      break;
    default: break;
  }
  recorder.reset();
  // Los que siguen pedirían el selector otra vez: se cancelan también.
  if (app && app->ok()) repaint |= app->cancelQueued();
  finishRequest(repaint);
}

// --- El trabajo de cada fase ----------------------------------------------------

void LuaAppsActivity::performTranscribe() {
  std::string text;
  std::string detail;
  requestMade = true;
  const unsigned long t0 = millis();
  const bool ok = recorder && SpeechToText::transcribe(*recorder, text, detail);
  WiFi.setSleep(true);
  recorder.reset();
  if (!ok) {
    LOG_ERR(TAG, "%s: no se transcribió: %s (%lu ms)", app->name().c_str(), detail.c_str(), millis() - t0);
  } else {
    LOG_INF(TAG, "%s: escuchó \"%s\" (%lu ms)", app->name().c_str(), text.c_str(), millis() - t0);
  }
  finishRequest(app->onHeard(ok ? text.c_str() : nullptr));
}

// POST /api/apps/call?lang=xx {app, service, args}
void LuaAppsActivity::performCall() {
  WiFi.setSleep(false);
  requestMade = true;
  std::string body = "{\"app\":\"" + app->appId() + "\",\"service\":\"" + current.a + "\",\"args\":" + current.b + "}";
  ServerClient::Response resp;
  const unsigned long t0 = millis();
  const ServerClient::Result r =
      SERVER_CLIENT.postJson(std::string("/api/apps/call?lang=") + uiLanguageCode(), body, resp, CALL_TIMEOUT_MS);
  const unsigned long ms = millis() - t0;
  WiFi.setSleep(true);
  body.clear();
  if (r != ServerClient::Result::Ok) {
    char detail[64];
    snprintf(detail, sizeof(detail), "sin conexión con el servidor (%d)", resp.status);
    LOG_ERR(TAG, "%s: %s falló: %s %d (%lu ms)", app->name().c_str(), current.a.c_str(),
            ServerClient::resultName(r), resp.status, ms);
    finishRequest(app->onReplyError(current.id, detail));
    return;
  }
  LOG_INF(TAG, "%s: %s -> %u B (%lu ms)%s", app->name().c_str(), current.a.c_str(), (unsigned)resp.body.size(), ms,
          ms > 15000 ? " — LENTO" : "");
  finishRequest(app->onReply(current.id, resp.body));
}

// GET /api/apps/file/<id> derecho a la tarjeta, con el Bearer del aparato.
void LuaAppsActivity::performDownload() {
  const std::string dir = current.c == "books" ? app->booksDir() : app->dataDir();
  Storage.ensureDirectoryExists(dir.c_str());
  const std::string dest = dir + "/" + current.b;
  const std::string url = SERVER_STORE.getBaseUrl() + "/api/apps/file/" + current.a;
  WiFi.setSleep(false);
  requestMade = true;
  const unsigned long t0 = millis();
  const HttpDownloader::DownloadError err = HttpDownloader::downloadToFile(
      url, dest, nullptr, nullptr, "", "", "Bearer " + SERVER_STORE.getToken());
  WiFi.setSleep(true);
  const unsigned long ms = millis() - t0;
  if (err != HttpDownloader::OK) {
    char detail[64];
    snprintf(detail, sizeof(detail), "descarga fallida (%d)", static_cast<int>(err));
    LOG_ERR(TAG, "%s: descarga %s -> %s falló (%d, %lu ms)", app->name().c_str(), current.a.c_str(), dest.c_str(),
            static_cast<int>(err), ms);
    finishRequest(app->onReplyError(current.id, detail));
    return;
  }
  const size_t bytes = fileSize(dest);
  LOG_INF(TAG, "%s: bajó %s (%u B, %lu ms)", app->name().c_str(), dest.c_str(), (unsigned)bytes, ms);
  finishRequest(app->onReplyBytes(current.id, bytes));
}

// El visor paginado del sistema con un archivo de la app. Lee hasta VIEW_CAP;
// el visor pagina solo. Al volver, la app recupera la pantalla y se repinta.
void LuaAppsActivity::openViewer() {
  const std::string path = app->dataDir() + "/" + current.a;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) {
    finishRequest(false);
    return;
  }
  const size_t total = f.size();
  const size_t size = std::min(total, LuaApp::VIEW_CAP);
  std::string text;
  text.resize(size);
  const int got = size ? f.read(&text[0], size) : 0;
  f.close();
  if (got <= 0) {
    finishRequest(false);
    return;
  }
  text.resize(static_cast<size_t>(got));
  if (size < total) {
    LOG_INF(TAG, "%s: cp.view(%s) recortado a %u KB", app->name().c_str(), current.a.c_str(),
            (unsigned)(LuaApp::VIEW_CAP / 1024));
  }
  auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, current.b, std::move(text));
  if (!viewer) {
    finishRequest(false);
    return;
  }
  phase = Phase::Viewing;
  startActivityForResult(std::move(viewer), [this](const ActivityResult&) {
    // El manager repinta solo al volver: la app vuelve a la pantalla.
    finishRequest(false);
  });
}

// Abrir el EPUB en el lector de CrossPoint. La app se cierra: el lector
// necesita el heap entero, y si en esta sesión hubo TLS ese heap está
// fragmentado, así que se va por el mismo camino que Preguntarle al libro
// (reinicio silencioso con destino el lector). Sin TLS de por medio se abre
// directo con la radio apagada. Al cerrar el libro se vuelve al hub, no acá.
void LuaAppsActivity::openBook() {
  const std::string path = app->booksDir() + "/" + current.a;
  LOG_INF(TAG, "%s: abre %s en el lector", app->name().c_str(), path.c_str());
  app.reset();
  phase = Phase::Idle;
  state = LIST;
  shutdownRadio();
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  if (requestMade) {
    silentRestartToReader();
    return;
  }
  activityManager.goToReader(path);
}

// --- Pintado -------------------------------------------------------------------

std::vector<std::string> LuaAppsActivity::wrapLines(const int fontId, const std::string& text, const int width,
                                                    const int maxLines) const {
  std::vector<std::string> lines;
  std::string line;
  size_t pos = 0;
  while (pos < text.size() && static_cast<int>(lines.size()) < maxLines) {
    size_t next = text.find(' ', pos);
    if (next == std::string::npos) next = text.size();
    const std::string word = text.substr(pos, next - pos);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && renderer.getTextWidth(fontId, candidate.c_str(), EpdFontFamily::BOLD) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    pos = next + 1;
  }
  if (!line.empty() && static_cast<int>(lines.size()) < maxLines) lines.push_back(line);
  // Lo que no entró se marca en el último renglón.
  if (pos < text.size() && !lines.empty()) {
    lines.back() = renderer.truncatedText(fontId, (lines.back() + "…").c_str(), width, EpdFontFamily::BOLD);
  }
  return lines;
}

// La pantalla de escucha: la pregunta de la app arriba, "Escuchando…" en el
// medio y la barra con lo que hacen los botones. Lo mismo que ve el usuario en
// Hablar, así una app no tiene que inventar la suya.
void LuaAppsActivity::renderListening() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, app->title().c_str());
  int y = metrics.topPadding + metrics.headerHeight + 24;
  const int lineH = renderer.getTextHeight(UI_12_FONT_ID) + 6;
  for (const std::string& line : wrapLines(UI_12_FONT_ID, current.a, pageWidth - 2 * SIDE, 4)) {
    renderer.drawText(UI_12_FONT_ID, SIDE, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineH;
  }
  renderer.drawCenteredText(UI_14_FONT_ID, mid - 10, tr(STR_LUA_LISTENING), true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_LUA_LISTEN_END), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::renderWaiting(const char* text) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, app->title().c_str());
  if (phase == Phase::Connecting) {
    FriendlyWifi::drawStatus(renderer, wifi, mid);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, text, true, EpdFontFamily::BOLD);
  }
  // Atrás cancela lo que todavía no salió; en el aire no hay botones.
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::renderList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  // El mismo nombre que el mosaico del hub: "Juegos" que abría "Apps de la tarjeta"
  // parecía otra pantalla. Ahora las dos dicen "Apps".
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_GAMES));

  if (apps.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20, tr(STR_LUA_NONE), true);
    renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12, LuaApp::dir(), true);
  } else {
    const int rows = visibleRows();
    const int top = listui::contentTop();
    const int rowW = listui::contentWidth(renderer);
    for (int row = 0; row < rows && scroll + row < static_cast<int>(apps.size()); ++row) {
      const int i = scroll + row;
      const int y = top + row * listui::ROW2_H;
      listui::RowSpec spec;
      // Título y descripción del comentario que abre el archivo; nunca la ruta,
      // que es un dato de la tarjeta y no de la app ("Apps/reloj.lua" no es
      // un nombre).
      spec.title = apps[i].title.c_str();
      spec.detail = apps[i].description.c_str();
      spec.selected = i == selected;
      spec.bold = true;
      listui::row(renderer, SIDE, y, rowW, listui::ROW2_H, spec);
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), apps.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::renderError() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LUA_ERROR));

  int y = metrics.topPadding + metrics.headerHeight + 32;
  if (app) {
    renderer.drawText(UI_12_FONT_ID, SIDE, y, app->title().c_str(), true, EpdFontFamily::BOLD);
    y += 40;
    // El mensaje del intérprete tal cual: es lo único que le dice al que
    // escribió la app dónde está el problema, y viene con archivo y línea.
    std::string rest = app->error();
    const int textW = pageWidth - 2 * SIDE;
    while (!rest.empty() && y < renderer.getScreenHeight() - metrics.buttonHintsHeight - 40) {
      const std::string shown = renderer.truncatedText(SMALL_FONT_ID, rest.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, SIDE, y, shown.c_str());
      y += 22;
      const size_t cut = shown.size() >= 3 ? shown.size() - 3 : shown.size();
      if (shown.size() < rest.size() && cut > 0 && cut < rest.size()) {
        rest = rest.substr(cut);
      } else {
        break;
      }
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LuaAppsActivity::render(RenderLock&&) {
  if (state == RUNNING && app && app->ok()) {
    switch (phase) {
      case Phase::Listening: renderListening(); break;
      case Phase::Connecting: renderWaiting(""); break;
      case Phase::Transcribing:
      case Phase::Calling: renderWaiting(tr(STR_LUA_WAIT_SERVER)); break;
      case Phase::Downloading: renderWaiting(tr(STR_LUA_DOWNLOADING)); break;
      case Phase::Viewing: return;  // el visor pinta
      case Phase::Idle:
        // La pantalla se le da limpia a la app y el refresco lo decide el
        // firmware: una app no elige cuándo se refresca el panel.
        renderer.clearScreen();
        app->onDraw();
        break;
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (state == FAILED) {
    renderError();
  } else {
    renderList();
  }
  // PARCIAL, como todas nuestras listas. Acá había un HALF_REFRESH fijo, o sea
  // que CADA movimiento de la palanca pagaba una onda de ~1,8 s con su
  // parpadeo: la lista de apps era la única pantalla del aparato que hacía eso,
  // y encima al volver de una app (que sí es un cuadro distinto) se sumaba otro.
  // La cadencia de limpiezas la decide el coordinador del panel — un completo
  // cada doce parciales — y no cada pantalla por su cuenta; para eso existe.
  renderer.displayBuffer();
}
