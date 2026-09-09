#include "PhotosActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/Selection.h"

namespace {
constexpr const char* TAG = "WALLP";
constexpr const char* DIR = "/Photos";
constexpr int ROW_H = 40;
constexpr int SIDE = 20;
constexpr unsigned long REFRESH_HOLD_MS = 1200;

bool isBmp(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".bmp") == 0;
}
}  // namespace

void PhotosActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists(DIR);
  scanLocal();
  state = LIST;
  requestUpdate();
  // La lista del servidor se busca sola solo cuando no hay nada en la SD. Con
  // fotos ya bajadas, elegir el fondo no necesita red: así salir de acá no
  // reinicia el aparato (toda pantalla que levanta WiFi hace silentRestart al
  // salir). Para traer las nuevas está Atrás mantenido.
  if (photos.empty() && SERVER_STORE.hasToken()) {
    pending = REFRESH;
    ensureConnected();
  }
}

void PhotosActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void PhotosActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

bool PhotosActivity::isWallpaper(const Photo& photo) const {
  return !HUB_STORE.wallpaperPath.empty() && HUB_STORE.wallpaperPath == photo.path;
}

// Everything already on the card: what was downloaded before plus anything the
// user copied there by hand.
void PhotosActivity::scanLocal() {
  photos.clear();
  auto dir = Storage.open(DIR);
  if (!dir || !dir.isDirectory()) return;
  dir.rewindDirectory();
  char name[128];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() || name[0] == '.' || !isBmp(name)) continue;
    Photo p;
    p.path = std::string(DIR) + "/" + name;
    p.name = name;
    p.name.resize(p.name.size() - 4);
    // Downloaded ones are "<id>.bmp"; a sidecar keeps the original name.
    p.id = p.name;
    p.local = true;
    HalFile meta;
    if (Storage.openFileForRead(TAG, std::string(DIR) + "/" + p.id + ".txt", meta)) {
      std::string title;
      title.resize(meta.size());
      const int got = meta.read(&title[0], title.size());
      meta.close();
      if (got > 0) p.name = title;
    }
    photos.push_back(std::move(p));
  }
  std::sort(photos.begin(), photos.end(), [](const Photo& a, const Photo& b) { return a.id > b.id; });
  if (index >= rowCount()) index = 0;
}

bool PhotosActivity::fetchList() {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/photos", resp);
  if (r != ServerClient::Result::Ok) {
    lastError = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/photos: %s", lastError.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  for (JsonVariantConst pv : doc["photos"].as<JsonArrayConst>()) {
    Photo p;
    p.id = pv["id"] | "";
    p.name = pv["name"] | "";
    if (p.id.empty()) continue;
    p.path = std::string(DIR) + "/" + p.id + ".bmp";
    p.local = Storage.exists(p.path.c_str());
    const bool known = std::any_of(photos.begin(), photos.end(), [&](const Photo& e) { return e.id == p.id; });
    if (!known) photos.push_back(std::move(p));
  }
  std::sort(photos.begin(), photos.end(), [](const Photo& a, const Photo& b) { return a.id > b.id; });
  return true;
}

bool PhotosActivity::download(Photo& photo) {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/photos/file?id=" + photo.id, resp);
  if (r != ServerClient::Result::Ok) {
    lastError = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/photos/file: %s", lastError.c_str());
    return false;
  }
  if (resp.body.size() < 100) return false;
  HalFile f;
  if (!Storage.openFileForWrite(TAG, photo.path, f)) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
  f.close();
  if (written != resp.body.size()) return false;
  if (!photo.name.empty()) {
    HalFile meta;
    if (Storage.openFileForWrite(TAG, std::string(DIR) + "/" + photo.id + ".txt", meta)) {
      meta.write(reinterpret_cast<const uint8_t*>(photo.name.data()), photo.name.size());
      meta.close();
    }
  }
  photo.local = true;
  LOG_INF(TAG, "%s: %u bytes", photo.id.c_str(), (unsigned)resp.body.size());
  return true;
}

// OK sobre una fila: "Ninguno" saca el fondo en el acto, una foto abre la vista
// previa (bajándola antes si todavía no está en la SD).
void PhotosActivity::confirmCurrent() {
  const int i = photoAt(index);
  if (i < 0) {
    setWallpaper(nullptr);
    return;
  }
  if (i >= static_cast<int>(photos.size())) return;
  if (!photos[i].local) {
    pending = DOWNLOAD;
    ensureConnected();
    return;
  }
  state = PREVIEW;
  requestUpdate();
}

void PhotosActivity::setWallpaper(const Photo* photo) {
  if (photo) {
    HUB_STORE.wallpaperPath = photo->path;
    HUB_STORE.wallpaperName = photo->name;
  } else {
    HUB_STORE.wallpaperPath.clear();
    HUB_STORE.wallpaperName.clear();
  }
  HUB_STORE.saveToFile();
  LOG_INF(TAG, "fondo: %s", photo ? photo->path.c_str() : "ninguno");
  state = LIST;
  requestUpdate();
}

void PhotosActivity::ensureConnected() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void PhotosActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = LOADING;
  requestUpdate();
}

void PhotosActivity::loop() {
  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      const Pending p = pending;
      pending = NONE;
      if (p == REFRESH) {
        const bool ok = fetchList();
        WiFi.setSleep(true);
        if (!ok) {
          // La lista no se pudo traer: se sigue mostrando lo que hay en la SD.
          LOG_ERR(TAG, "lista: %s", lastError.c_str());
        }
        state = LIST;
        requestUpdate();
      } else if (p == DOWNLOAD && photoAt(index) >= 0 && photoAt(index) < static_cast<int>(photos.size())) {
        const bool ok = download(photos[photoAt(index)]);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, lastError.empty() ? std::string(tr(STR_DOWNLOAD_FAILED)) : lastError);
          break;
        }
        state = PREVIEW;
        requestUpdate();
      } else {
        state = LIST;
        requestUpdate();
      }
      break;
    }
    case LIST: {
      const int count = rowCount();
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, REFRESH_HOLD_MS)) {
        pending = REFRESH;
        ensureConnected();
        break;
      }
      buttonNavigator.onNext([&] {
        index = ButtonNavigator::nextIndex(index, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        index = ButtonNavigator::previousIndex(index, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        confirmCurrent();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      break;
    }
    case PREVIEW: {
      // Sin navegación acá: cada foto son dos formas de onda y navegar en la
      // previa dejaba la pantalla parpadeando. Se elige o se vuelve a la lista.
      const int i = photoAt(index);
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        setWallpaper(i >= 0 && i < static_cast<int>(photos.size()) ? &photos[i] : nullptr);
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = LIST;
        requestUpdate();
      }
      break;
    }
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = LIST;
        requestUpdate();
      }
      break;
    case CONNECTING:
      break;
  }
}

// Pantalla completa, centrada, con los 4 grises de verdad. Ojo: una sola pasada
// en modo BW pinta de negro TODO lo que no sea blanco puro (drawBitmap en BW:
// negro si val < 3, nada si val == 3), así que una foto difuminada a 4 niveles
// salía como una mancha negra. Hay que correr el pipeline de gris del SDK:
// base en blanco y negro + plano LSB + plano MSB + displayGrayBuffer.
bool PhotosActivity::drawFullScreenPhoto(GfxRenderer& renderer, const std::string& path,
                                         const std::function<void()>& baseOverlay) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "no se pudo abrir %s", path.c_str());
    return false;
  }
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR(TAG, "BMP inválido: %s", path.c_str());
    return false;
  }

  int x = 0, y = 0;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      y = std::round((pageHeight - pageWidth / ratio) / 2);
    } else {
      x = std::round((pageWidth - pageHeight * ratio) / 2);
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  if (!bitmap.hasGreyscale()) {
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    if (baseOverlay) baseOverlay();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  // Los tres planos se arman ANTES de tocar la pantalla. Leer y escalar el BMP
  // desde la SD tarda casi un segundo por pasada, y hacerlo entre el destello
  // de la base y el empujón de gris dejaba la versión en blanco y negro a la
  // vista todo ese rato (los "cuatro destellos" que se veían). Con los planos
  // guardados en PSRAM, la base y el gris salen pegados y se ve una sola
  // aparición. Si no hay PSRAM para los planos, se cae al orden clásico.
  const size_t bufferSize = renderer.getBufferSize();
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  uint8_t* lsb = static_cast<uint8_t*>(heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* msb = lsb ? static_cast<uint8_t*>(heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
  const bool staged = lsb && msb && frameBuffer;

  // Cada pasada dibuja lo mismo: la foto y lo que le va encima. El overlay tiene
  // que entrar también en los planos de gris, si no el empujón de gris lo borra
  // (dibuja TODA la pantalla) y la barra de botones se pierde.
  const auto drawPass = [&] {
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    if (baseOverlay) baseOverlay();
  };

  if (staged) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    drawPass();
    memcpy(lsb, frameBuffer, bufferSize);

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    drawPass();
    memcpy(msb, frameBuffer, bufferSize);

    bitmap.rewindToData();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // La base tiene que ser HALF: la LUT del empujón de gris está calibrada
  // contra el estado que deja esa forma de onda.
  renderer.clearScreen();
  drawPass();
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  if (staged) {
    memcpy(frameBuffer, lsb, bufferSize);
    renderer.copyGrayscaleLsbBuffers();
    memcpy(frameBuffer, msb, bufferSize);
    renderer.copyGrayscaleMsbBuffers();
  } else {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    drawPass();
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    drawPass();
    renderer.copyGrayscaleMsbBuffers();
  }

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  heap_caps_free(lsb);
  heap_caps_free(msb);
  return true;
}

// Vista previa de la foto marcada: así se va a ver de fondo. El cartel de
// "Cargando..." tapa la espera (abrir el BMP y armar los tres planos tarda unos
// segundos) para que no parezca que la pantalla se colgó, y la foto aparece de
// una sola vez. La barra de botones va como overlay de las tres pasadas: encima
// del empujón de gris no se puede pintar nada, porque el framebuffer ya no tiene
// la imagen sino el plano MSB.
void PhotosActivity::drawPreview() {
  const int i = photoAt(index);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WALLPAPER_USE), "", "");
  const auto hints = [&] { GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4); };
  if (i >= 0 && i < static_cast<int>(photos.size())) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
    if (drawFullScreenPhoto(renderer, photos[i].path, hints)) return;
  }
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_FILE_OPEN_FAILED));
  hints();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void PhotosActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  if (state == PREVIEW) {
    drawPreview();  // se encarga de su propio refresco (pipeline de grises)
    return;
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLPAPER));
  switch (state) {
    case LIST: {
      const int count = rowCount();
      // Qué hay elegido ahora, arriba de todo: es lo primero que se pregunta el
      // que entra acá.
      const std::string current =
          std::string(tr(STR_WALLPAPER_CURRENT)) + ": " +
          (HUB_STORE.wallpaperPath.empty() ? std::string(tr(STR_NONE_OPT)) : HUB_STORE.wallpaperName);
      const int currentY = metrics.topPadding + metrics.headerHeight + 6;
      renderer.drawText(SMALL_FONT_ID, SIDE,
                        currentY,
                        renderer.truncatedText(SMALL_FONT_ID, current.c_str(), pageWidth - 2 * SIDE).c_str());

      const int top = currentY + renderer.getLineHeight(SMALL_FONT_ID) + 8;
      const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
      itemsPerPage = std::max(1, (bottom - top) / ROW_H);
      const int first = (index / itemsPerPage) * itemsPerPage;
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * ROW_H;
        const bool sel = i == index;
        const int photoIndex = photoAt(i);
        if (sel) drawSelectionRow(renderer, SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4);
        const char* label = photoIndex < 0 ? tr(STR_NONE_OPT) : photos[photoIndex].name.c_str();
        renderer.drawText(UI_12_FONT_ID, SIDE, y + 8,
                          renderer.truncatedText(UI_12_FONT_ID, label, pageWidth - 2 * SIDE - 30).c_str(), SELECTION_INK);
        // Punto a la derecha = ésta es la que está de fondo.
        const bool marked = photoIndex < 0 ? HUB_STORE.wallpaperPath.empty() : isWallpaper(photos[photoIndex]);
        if (marked) renderer.fillRoundedRect(pageWidth - SIDE - 12, y + ROW_H / 2 - 8, 12, 12, 6, Color::Black);
      }
      if (photos.empty()) renderer.drawCenteredText(UI_10_FONT_ID, mid + 40, tr(STR_WALLPAPER_EMPTY));
      char pages[16];
      snprintf(pages, sizeof(pages), "%d/%d", index / itemsPerPage + 1, (count + itemsPerPage - 1) / itemsPerPage);
      renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom + 4, pages);
      renderer.drawText(SMALL_FONT_ID, SIDE, bottom + 4, tr(STR_PHOTOS_REFRESH_HINT));
      break;
    }
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_PHOTOS_LOADING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, failureDetail.c_str());
      break;
    default:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
