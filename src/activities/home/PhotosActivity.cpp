#include "PhotosActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "PHOTOS";
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
  if (index >= static_cast<int>(photos.size())) index = 0;
}

bool PhotosActivity::fetchList() {
  ServerClient::Response resp;
  if (SERVER_CLIENT.get("/api/photos", resp) != ServerClient::Result::Ok) return false;
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
  if (SERVER_CLIENT.get("/api/photos/file?id=" + photo.id, resp) != ServerClient::Result::Ok) return false;
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

void PhotosActivity::openCurrent() {
  if (photos.empty()) return;
  if (!photos[index].local) {
    pending = DOWNLOAD;
    ensureConnected();
    return;
  }
  state = VIEW;
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
          fail(StrId::STR_ASK_FAILED, "list");
          break;
        }
        state = LIST;
        requestUpdate();
      } else if (p == DOWNLOAD && index < static_cast<int>(photos.size())) {
        const bool ok = download(photos[index]);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, "download");
          break;
        }
        state = VIEW;
        requestUpdate();
      } else {
        state = LIST;
        requestUpdate();
      }
      break;
    }
    case LIST: {
      const int count = static_cast<int>(photos.size());
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, REFRESH_HOLD_MS)) {
        pending = REFRESH;
        ensureConnected();
        break;
      }
      buttonNavigator.onNext([&] {
        if (count > 0) index = ButtonNavigator::nextIndex(index, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) index = ButtonNavigator::previousIndex(index, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openCurrent();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) activityManager.goHome();
      break;
    }
    case VIEW: {
      const int count = static_cast<int>(photos.size());
      buttonNavigator.onNext([&] {
        index = ButtonNavigator::nextIndex(index, count);
        if (!photos[index].local) {
          pending = DOWNLOAD;
          ensureConnected();
        } else {
          requestUpdate();
        }
      });
      buttonNavigator.onPrevious([&] {
        index = ButtonNavigator::previousIndex(index, count);
        if (!photos[index].local) {
          pending = DOWNLOAD;
          ensureConnected();
        } else {
          requestUpdate();
        }
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

// Full screen, centred, no chrome except the hint row: same path the BMP
// viewer uses (the SDK reader handles the 2 bpp palette natively).
void PhotosActivity::drawPhoto() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  HalFile file;
  if (!Storage.openFileForRead(TAG, photos[index].path, file)) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    return;
  }
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    return;
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
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
}

void PhotosActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  if (state == VIEW && !photos.empty()) {
    drawPhoto();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);  // photos deserve the clean waveform
    return;
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HUB_PHOTOS));
  switch (state) {
    case LIST: {
      const int count = static_cast<int>(photos.size());
      const int top = metrics.topPadding + metrics.headerHeight + 10;
      const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
      itemsPerPage = std::max(1, (bottom - top) / ROW_H);
      const int first = count > 0 ? (index / itemsPerPage) * itemsPerPage : 0;
      if (count == 0) renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, tr(STR_PHOTOS_EMPTY));
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * ROW_H;
        const bool sel = i == index;
        if (sel) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
        renderer.drawText(UI_12_FONT_ID, SIDE, y + 8,
                          renderer.truncatedText(UI_12_FONT_ID, photos[i].name.c_str(), pageWidth - 2 * SIDE - 30).c_str(), !sel);
        if (photos[i].local) renderer.fillRect(pageWidth - SIDE - 6, y + ROW_H / 2 - 5, 6, 6, !sel);
      }
      char pages[16];
      snprintf(pages, sizeof(pages), "%d/%d", count ? index / itemsPerPage + 1 : 0, (count + itemsPerPage - 1) / itemsPerPage);
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
