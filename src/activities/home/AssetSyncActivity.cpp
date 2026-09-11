#include "AssetSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <StreamingJsonParser.h>
#include <WiFi.h>
#include <ws397_version.h>  // ws397: el número de build vive acá, no en un -D

#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstdlib>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "ASSETS";
constexpr const char* LOCAL_MANIFEST = "/.crosspoint/assets.json";
constexpr int PARTIALS_BEFORE_CLEAN = 12;   // regla del panel: refresco limpio cada 10-15 parciales
constexpr unsigned long PAINT_EVERY_MS = 1500;
constexpr int MAX_TRIES = 3;                // intentos por archivo antes de saltearlo
constexpr size_t MAX_FILE_BYTES = 512 * 1024;  // sin streaming, el archivo entra entero en memoria
constexpr int SAVE_EVERY = 20;              // cada cuántos archivos se guarda el manifiesto local

// La ruta que manda el servidor: absoluta si arranca con "/", si no cuelga de
// nuestro directorio. Así la Biblia cae en /.crosspoint/bible/<lang>/bNN.txt,
// que es donde ya la busca BibleActivity.
std::string sdPath(const std::string& path) {
  if (path.empty()) return path;
  if (path[0] == '/') return path;
  return std::string("/.crosspoint/") + path;
}

// Crea los directorios intermedios de un archivo ("/.crosspoint/cards/img/x.bmp").
void ensureParentDirs(const std::string& path) {
  size_t at = path.find('/', 1);
  while (at != std::string::npos) {
    Storage.ensureDirectoryExists(path.substr(0, at).c_str());
    at = path.find('/', at + 1);
  }
}

// Los primeros 16 hex del sha256, que es lo que manda el manifiesto
// (server/src/assets.ts, sha16()). mbedtls ya viene con el ESP-IDF.
std::string sha16Of(const std::string& data) {
  unsigned char out[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0 ||
      mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) != 0 ||
      mbedtls_sha256_finish(&ctx, out) != 0) {
    mbedtls_sha256_free(&ctx);
    return {};
  }
  mbedtls_sha256_free(&ctx);
  char hex[17];
  for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
  hex[16] = 0;
  return std::string(hex);
}

std::string readWholeFile(const char* path) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return {};
  std::string raw;
  raw.resize(f.size());
  const int got = raw.empty() ? 0 : f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return {};
  raw.resize(static_cast<size_t>(got));
  return raw;
}

// Busca el sha de un id dentro del manifiesto local crudo, sin armar un mapa:
// son unos cientos de ítems y buscar en un texto de unas decenas de KB es
// barato al lado de abrir la tarjeta.
std::string localShaOf(const std::string& raw, const std::string& id) {
  if (raw.empty() || id.empty()) return {};
  const std::string needle = "\"" + id + "\":\"";
  const size_t at = raw.find(needle);
  if (at == std::string::npos) return {};
  const size_t start = at + needle.size();
  const size_t end = raw.find('"', start);
  if (end == std::string::npos) return {};
  return raw.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Lector del manifiesto del servidor
// {"version":"...","items":[{"id","kind","path","bytes","sha"}, ...]}
//
// Va por el parser SAX (`StreamingJsonParser`) y no por ArduinoJson: son cientos
// de ítems y armar el documento entero en memoria, con el WiFi y el TLS arriba,
// es justo lo que no hay.
// ---------------------------------------------------------------------------
struct ManifestSink {
  std::string version;
  std::vector<AssetSyncActivity::Item>* out = nullptr;
  int depth = 0;         // profundidad de objetos
  bool inItems = false;  // dentro del array "items"
  std::string key;       // última clave vista
  AssetSyncActivity::Item cur;
};

ManifestSink* sink(void* ctx) { return static_cast<ManifestSink*>(ctx); }

void mOnKey(void* ctx, const char* key, const size_t len) { sink(ctx)->key.assign(key, len); }

void mOnString(void* ctx, const char* value, const size_t len) {
  ManifestSink* s = sink(ctx);
  const std::string v(value, len);
  if (s->depth == 1 && s->key == "version") {
    s->version = v;
  } else if (s->inItems && s->depth == 2) {
    if (s->key == "id") s->cur.id = v;
    else if (s->key == "kind") s->cur.kind = v;
    else if (s->key == "path") s->cur.path = v;
    else if (s->key == "sha") s->cur.sha = v;
    else if (s->key == "bytes") s->cur.bytes = strtoul(v.c_str(), nullptr, 10);  // por si viene como texto
  }
  s->key.clear();
}

void mOnNumber(void* ctx, const char* value, size_t) {
  ManifestSink* s = sink(ctx);
  if (s->inItems && s->depth == 2 && s->key == "bytes") {
    s->cur.bytes = strtoul(value, nullptr, 10);
  } else if (s->depth == 1 && s->key == "version" && s->version.empty()) {
    s->version = value;  // una versión numérica también sirve
  }
  s->key.clear();
}

void mOnBool(void*, bool) {}
void mOnNull(void*) {}

void mOnObjectStart(void* ctx) {
  ManifestSink* s = sink(ctx);
  s->depth++;
  if (s->inItems && s->depth == 2) s->cur = AssetSyncActivity::Item();
  s->key.clear();
}

void mOnObjectEnd(void* ctx) {
  ManifestSink* s = sink(ctx);
  if (s->inItems && s->depth == 2 && !s->cur.id.empty() && !s->cur.path.empty()) {
    s->out->push_back(s->cur);
  }
  s->depth--;
  s->key.clear();
}

void mOnArrayStart(void* ctx) {
  ManifestSink* s = sink(ctx);
  if (s->depth == 1 && s->key == "items") s->inItems = true;
  s->key.clear();
}

void mOnArrayEnd(void* ctx) {
  ManifestSink* s = sink(ctx);
  s->inItems = false;
  s->key.clear();
}

// "3,4 MB" a partir de bytes (sin coma flotante, que no hace falta).
std::string megabytes(const uint32_t bytes) {
  const uint32_t kb = bytes / 1024;
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu,%lu MB", static_cast<unsigned long>(kb / 1024),
           static_cast<unsigned long>((kb % 1024) * 10 / 1024));
  return buf;
}
}  // namespace

// ---------------------------------------------------------------------------
// Estado del paquete, para las pantallas que dependen de él
// ---------------------------------------------------------------------------

void AssetSyncActivity::markPending() {
  HUB_STORE.assetsPending = true;
  HUB_STORE.saveToFile();
}

bool AssetSyncActivity::isPending() { return HUB_STORE.assetsPending; }

bool AssetSyncActivity::hasPackage() { return !HUB_STORE.assetsVersion.empty(); }

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------

void AssetSyncActivity::onEnter() {
  Activity::onEnter();
  lang = uiLanguageCode();
  Storage.ensureDirectoryExists("/.crosspoint");
  if (wifiReady && WiFi.status() == WL_CONNECTED) {
    state = MANIFEST;  // el pedido sale del loop(), así la pantalla se pinta primero
    requestUpdate();
    return;
  }
  beginConnect();
}

void AssetSyncActivity::onExit() {
  Activity::onExit();
  // Solo reinicia el que levantó el WiFi: si nos abrió la actualización de
  // firmware, ella se encarga (y reiniciar dos veces sería absurdo).
  if (ownsWifi && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void AssetSyncActivity::beginConnect() {
  ownsWifi = true;
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) {
    pumpConnect();
    return;
  }
  requestUpdate();
}

void AssetSyncActivity::pumpConnect() {
  if (wifiPicker) return;  // la pantalla de selección tiene el foco
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase phase = wifi.pump();
  if (phase == FriendlyWifi::Phase::Connected) {
    onWifiReady(true);
    return;
  }
  if (phase == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             onWifiReady(!result.isCancelled);
                           });
    return;
  }
  if (wifi.revision() != rev) requestUpdate();
}

void AssetSyncActivity::onWifiReady(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = MANIFEST;
  forceClean = true;
  requestUpdate();
}

void AssetSyncActivity::fail(const StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  forceClean = true;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Manifiesto
// ---------------------------------------------------------------------------

void AssetSyncActivity::fetchManifest() {
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  WiFi.setSleep(false);
  ServerClient::Response resp;
  const std::string path =
      "/api/assets/manifest?lang=" + lang + "&v=" + urlEncode(WS397_VERSION);
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok) {
    WiFi.setSleep(true);
    // Un servidor sin paquete de contenido (404) no es una falla: no hay nada
    // que bajar y punto.
    if (resp.status == 404) {
      upToDate = true;
      state = DONE;
      forceClean = true;
      requestUpdate();
      return;
    }
    fail(StrId::STR_ASSETS_MANIFEST_FAILED, ServerClient::resultName(r));
    return;
  }

  items.clear();
  ManifestSink handler;
  handler.out = &items;
  StreamingJsonParser parser(JsonCallbacks{&handler, mOnKey, mOnString, mOnNumber, mOnBool, mOnNull, mOnObjectStart,
                                           mOnObjectEnd, mOnArrayStart, mOnArrayEnd});
  parser.feed(resp.body.data(), resp.body.size());
  version = handler.version;
  resp.body.clear();
  resp.body.shrink_to_fit();  // el cuerpo del manifiesto es lo más grande que se toca acá

  if (items.empty()) {
    WiFi.setSleep(true);
    upToDate = true;
    state = DONE;
    forceClean = true;
    requestUpdate();
    return;
  }

  // Qué falta: lo que no está en la tarjeta o cambió de sha. El manifiesto local
  // solo anota lo que se escribió entero, así que una descarga cortada a la
  // mitad no queda anotada y se vuelve a bajar.
  localRaw = readWholeFile(LOCAL_MANIFEST);
  needCount = 0;
  needBytes = 0;
  for (Item& item : items) {
    const std::string have = localShaOf(localRaw, item.id);
    const std::string full = sdPath(item.path);
    const bool same = !have.empty() && have == item.sha && Storage.exists(full.c_str());
    item.present = same;
    item.needed = !same;
    if (item.needed) {
      needCount++;
      needBytes += item.bytes;
    }
  }
  localRaw.clear();
  localRaw.shrink_to_fit();
  LOG_INF(TAG, "manifiesto %s: %u archivos, %d por bajar", version.c_str(), (unsigned)items.size(), needCount);

  if (needCount == 0) {
    WiFi.setSleep(true);
    upToDate = true;
    finishDownload();
    return;
  }
  index = 0;
  tries = 0;
  state = DOWNLOADING;
  forceClean = true;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Descarga
// ---------------------------------------------------------------------------

bool AssetSyncActivity::downloadItem(const Item& item) {
  if (item.bytes > MAX_FILE_BYTES) {
    LOG_ERR(TAG, "%s: %lu bytes, demasiado grande", item.id.c_str(), static_cast<unsigned long>(item.bytes));
    return false;
  }
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/assets/file?id=" + urlEncode(item.id), resp);
  if (r != ServerClient::Result::Ok || resp.body.empty()) {
    LOG_ERR(TAG, "%s: %s (%d)", item.id.c_str(), ServerClient::resultName(r), resp.status);
    return false;
  }
  // El cuerpo tiene que ser EL archivo, no cualquier cosa de 200: antes
  // alcanzaba con que la escritura no fuera corta, se guardaba el sha ANUNCIADO
  // y en la sincronizacion siguiente el archivo ya figuraba al dia aunque
  // estuviera cortado o cambiado por el camino.
  if (item.bytes > 0 && resp.body.size() != item.bytes) {
    LOG_ERR(TAG, "%s: llegaron %u bytes y el manifiesto dice %lu", item.id.c_str(),
            static_cast<unsigned>(resp.body.size()), static_cast<unsigned long>(item.bytes));
    return false;
  }
  if (!item.sha.empty()) {
    const std::string got = sha16Of(resp.body);
    if (got.empty() || got != item.sha) {
      LOG_ERR(TAG, "%s: sha %s, se esperaba %s", item.id.c_str(), got.c_str(), item.sha.c_str());
      return false;
    }
  }
  const std::string full = sdPath(item.path);
  ensureParentDirs(full);
  HalFile f;
  if (!Storage.openFileForWrite(TAG, full, f)) {
    LOG_ERR(TAG, "no se pudo escribir %s", full.c_str());
    return false;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
  f.close();
  if (written != resp.body.size()) {
    Storage.remove(full.c_str());  // a medias no sirve: que se vuelva a bajar
    LOG_ERR(TAG, "escritura corta en %s", full.c_str());
    return false;
  }
  doneBytes += static_cast<uint32_t>(resp.body.size());
  return true;
}

// Un archivo por pasada del loop: la pantalla y los botones siguen vivos.
void AssetSyncActivity::downloadStep() {
  while (index < items.size() && !items[index].needed) index++;
  if (index >= items.size()) {
    finishDownload();
    return;
  }
  Item& item = items[index];
  if (downloadItem(item)) {
    item.needed = false;
    item.present = true;
    doneCount++;
    tries = 0;
    index++;
    if (++savedSince >= SAVE_EVERY) saveLocalManifest();
  } else if (++tries >= MAX_TRIES) {
    // Ese archivo no salió: se anota y se sigue con el resto, que es mejor que
    // dejar todo el paquete a medias por uno.
    failCount++;
    item.needed = false;
    tries = 0;
    index++;
  }
  paintProgress();
}

void AssetSyncActivity::finishDownload() {
  WiFi.setSleep(true);
  saveLocalManifest();
  HUB_STORE.assetsVersion = version;
  HUB_STORE.assetsLang = lang;
  HUB_STORE.assetsFiles = 0;
  for (const Item& item : items) {
    if (item.present) HUB_STORE.assetsFiles++;
  }
  // Solo deja de estar pendiente cuando bajó todo lo que faltaba.
  if (failCount == 0 && !stopped) HUB_STORE.assetsPending = false;
  HUB_STORE.saveToFile();
  LOG_INF(TAG, "paquete %s: %d bajados, %d fallados, %d en la tarjeta", version.c_str(), doneCount, failCount,
          HUB_STORE.assetsFiles);
  state = DONE;
  forceClean = true;
  requestUpdate();
}

// {"version":"...","lang":"es","items":{"<id>":"<sha>", ...}} escrito de a
// pedazos, sin armar el texto entero en memoria.
void AssetSyncActivity::saveLocalManifest() {
  savedSince = 0;
  HalFile f;
  if (!Storage.openFileForWrite(TAG, LOCAL_MANIFEST, f)) {
    LOG_ERR(TAG, "no se pudo guardar %s", LOCAL_MANIFEST);
    return;
  }
  std::string head = "{\"version\":\"" + version + "\",\"lang\":\"" + lang + "\",\"items\":{";
  f.write(reinterpret_cast<const uint8_t*>(head.data()), head.size());
  bool first = true;
  std::string chunk;
  chunk.reserve(1024);
  for (const Item& item : items) {
    if (!item.present) continue;
    if (!first) chunk += ",";
    first = false;
    chunk += "\"" + item.id + "\":\"" + item.sha + "\"";
    if (chunk.size() > 768) {
      f.write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
      chunk.clear();
    }
  }
  chunk += "}}";
  f.write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
  f.close();
}

int AssetSyncActivity::percent() const {
  if (needCount <= 0) return 100;
  const int done = doneCount + failCount;
  const int value = done * 100 / needCount;
  return value > 100 ? 100 : value;
}

void AssetSyncActivity::paintProgress(const bool force) {
  const int now = percent();
  if (!force && now == lastPercent && millis() - lastPaintAt < PAINT_EVERY_MS) return;
  lastPercent = now;
  lastPaintAt = millis();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void AssetSyncActivity::loop() {
  switch (state) {
    case CONNECTING:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      pumpConnect();
      break;
    case MANIFEST:
      fetchManifest();
      break;
    case DOWNLOADING:
      // Atrás corta y conserva lo bajado: la próxima vez sigue desde ahí.
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        stopped = true;
        finishDownload();
        return;
      }
      downloadStep();
      break;
    case DONE:
    case FAILED:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        finish();
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------

void AssetSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;
  const int textW = pageWidth - 40;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ASSETS_TITLE));
  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = "";

  switch (state) {
    case CONNECTING:
      FriendlyWifi::drawStatus(renderer, wifi, mid);
      break;
    case MANIFEST:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_ASSETS_CHECKING), true, EpdFontFamily::BOLD);
      backLabel = "";
      break;
    case DOWNLOADING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 76, tr(STR_ASSETS_DOWNLOADING), true, EpdFontFamily::BOLD);
      char line[96];
      snprintf(line, sizeof(line), "%d/%d %s", doneCount + failCount, needCount, tr(STR_ASSETS_FILES));
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 38, line);
      const std::string sizes = megabytes(doneBytes) + "  ·  " + megabytes(needBytes);
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 8, sizes.c_str());

      const int barW = pageWidth - 120;
      renderer.drawRect(60, mid + 26, barW, 14, true);
      const int fill = barW * percent() / 100;
      if (fill > 4) renderer.fillRect(62, mid + 28, fill - 4, 10, true);

      if (index < items.size()) {
        renderer.drawCenteredText(
            SMALL_FONT_ID, mid + 54,
            renderer.truncatedText(SMALL_FONT_ID, items[index].id.c_str(), textW).c_str());
      }
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 86, tr(STR_ASSETS_STOP_HINT));
      backLabel = tr(STR_ASSETS_STOP);
      break;
    }
    case DONE: {
      const StrId head = upToDate ? StrId::STR_ASSETS_UP_TO_DATE
                        : stopped ? StrId::STR_ASSETS_STOPPED
                                  : StrId::STR_ASSETS_DONE;
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, I18N.get(head), true, EpdFontFamily::BOLD);
      if (!upToDate) {
        char line[96];
        snprintf(line, sizeof(line), "%d %s  ·  %s", doneCount, tr(STR_ASSETS_FILES), megabytes(doneBytes).c_str());
        renderer.drawCenteredText(UI_10_FONT_ID, mid, line);
      }
      if (failCount > 0) {
        char line[96];
        snprintf(line, sizeof(line), "%d %s", failCount, tr(STR_ASSETS_FAILED_COUNT));
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 30, line);
      }
      if (stopped || failCount > 0) {
        renderer.drawCenteredText(SMALL_FONT_ID, mid + 64, tr(STR_ASSETS_RESUME_HINT));
      }
      confirmLabel = tr(STR_DONE);
      break;
    }
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 16,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), textW).c_str());
      }
      confirmLabel = tr(STR_DONE);
      break;
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Regla del panel: refresco limpio cada tantos parciales (y en cada cambio de
  // pantalla) o la tinta queda fantasmeada.
  const bool clean = forceClean || ++partialCount >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
