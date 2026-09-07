#include "NewsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "NEWS";
constexpr const char* DIR = "/.crosspoint/rss";
constexpr const char* CACHE = "/.crosspoint/rss/feeds.json";
constexpr int ROW_H = 44;
constexpr int SIDE = 20;
constexpr unsigned long REFRESH_HOLD_MS = 1200;
}  // namespace

void NewsActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists(DIR);
  if (loadCache()) {
    state = FEEDS;
    requestUpdate();
  } else {
    pending = REFRESH;
    ensureConnected();
  }
}

void NewsActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void NewsActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

bool NewsActivity::loadCache() {
  if (!Storage.exists(CACHE)) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, CACHE, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  feeds.clear();
  for (JsonVariantConst fv : doc["feeds"].as<JsonArrayConst>()) {
    Feed feed;
    feed.id = fv["id"] | 0;
    feed.name = fv["name"] | "";
    for (JsonVariantConst iv : fv["items"].as<JsonArrayConst>()) feed.items.push_back({iv["id"] | 0, iv["title"] | "", iv["when"] | ""});
    feeds.push_back(std::move(feed));
  }
  return !feeds.empty();
}

bool NewsActivity::fetchFeeds() {
  ServerClient::Response resp;
  if (SERVER_CLIENT.get("/api/rss", resp) != ServerClient::Result::Ok) return false;
  HalFile f;
  if (Storage.openFileForWrite(TAG, CACHE, f)) {
    f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
    f.close();
  }
  return loadCache();
}

std::string NewsActivity::articlePath(const int feed, const int item) const {
  return std::string(DIR) + "/a" + std::to_string(feed) + "-" + std::to_string(item) + ".txt";
}

bool NewsActivity::readArticle(const std::string& path, std::string& title, std::string& text) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return false;
  const size_t nl = raw.find('\n');
  title = raw.substr(0, nl);
  text = nl == std::string::npos ? "" : raw.substr(nl + 1);
  return !text.empty();
}

bool NewsActivity::fetchArticle(const int feed, const int item, std::string& title, std::string& text) {
  ServerClient::Response resp;
  if (SERVER_CLIENT.get("/api/rss/article?feed=" + std::to_string(feed) + "&item=" + std::to_string(item), resp) !=
      ServerClient::Result::Ok) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  title = doc["title"] | "";
  text = doc["text"] | "";
  if (text.empty()) return false;
  HalFile f;
  if (Storage.openFileForWrite(TAG, articlePath(feed, item), f)) {
    f.write(reinterpret_cast<const uint8_t*>(title.data()), title.size());
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    f.close();
  }
  return true;
}

void NewsActivity::openArticle() {
  const Feed& feed = feeds[feedIndex];
  if (feed.items.empty()) return;
  const Item& item = feed.items[itemIndex];
  std::string title, text;
  if (readArticle(articlePath(feed.id, item.id), title, text)) {
    showArticle(title, text);
    return;
  }
  pending = ARTICLE;
  ensureConnected();
}

void NewsActivity::showArticle(const std::string& title, const std::string& text) {
  state = READING;
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, title, text),
                         [this](const ActivityResult&) {
                           state = ITEMS;
                           requestUpdate();
                         });
}

void NewsActivity::ensureConnected() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void NewsActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = LOADING;
  requestUpdate();
}

void NewsActivity::loop() {
  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      const Pending p = pending;
      pending = NONE;
      if (p == REFRESH) {
        const bool ok = fetchFeeds();
        WiFi.setSleep(true);
        if (!ok) {
          fail(feeds.empty() ? StrId::STR_NEWS_NO_FEEDS : StrId::STR_ASK_FAILED);
          break;
        }
        state = FEEDS;
        feedIndex = 0;
        requestUpdate();
      } else if (p == ARTICLE) {
        std::string title, text;
        const bool ok = fetchArticle(feeds[feedIndex].id, feeds[feedIndex].items[itemIndex].id, title, text);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, "article");
          break;
        }
        showArticle(title, text);
      } else {
        state = FEEDS;
        requestUpdate();
      }
      break;
    }
    case FEEDS:
    case ITEMS: {
      const bool inFeeds = state == FEEDS;
      const int count = inFeeds ? static_cast<int>(feeds.size()) : static_cast<int>(feeds[feedIndex].items.size());
      int& index = inFeeds ? feedIndex : itemIndex;
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
        if (inFeeds) {
          if (!feeds.empty()) {
            state = ITEMS;
            itemIndex = 0;
            requestUpdate();
          }
        } else {
          openArticle();
        }
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        if (inFeeds) {
          activityManager.goHome();
        } else {
          state = FEEDS;
          requestUpdate();
        }
      }
      break;
    }
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (feeds.empty()) activityManager.goHome();
        else {
          state = FEEDS;
          requestUpdate();
        }
      }
      break;
    case CONNECTING:
    case READING:
      break;
  }
}

void NewsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  const std::string title = state == ITEMS ? feeds[feedIndex].name : std::string(tr(STR_HUB_NEWS));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  switch (state) {
    case FEEDS:
    case ITEMS: {
      const bool inFeeds = state == FEEDS;
      const int count = inFeeds ? static_cast<int>(feeds.size()) : static_cast<int>(feeds[feedIndex].items.size());
      const int selected = inFeeds ? feedIndex : itemIndex;
      const int top = metrics.topPadding + metrics.headerHeight + 10;
      const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
      itemsPerPage = std::max(1, (bottom - top) / ROW_H);
      const int first = count > 0 ? (selected / itemsPerPage) * itemsPerPage : 0;
      if (count == 0) renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, inFeeds ? tr(STR_NEWS_NO_FEEDS) : tr(STR_NEWS_NO_ITEMS));
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * ROW_H;
        const bool sel = i == selected;
        if (sel) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
        if (inFeeds) {
          const Feed& f = feeds[i];
          renderer.drawText(UI_12_FONT_ID, SIDE, y + 9, renderer.truncatedText(UI_12_FONT_ID, f.name.c_str(), pageWidth - 2 * SIDE - 40).c_str(), !sel);
          const std::string n = std::to_string(f.items.size());
          renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(UI_10_FONT_ID, n.c_str()), y + 12, n.c_str(), !sel);
        } else {
          const Item& it = feeds[feedIndex].items[i];
          const int whenW = it.when.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, it.when.c_str()) + 10;
          renderer.drawText(UI_10_FONT_ID, SIDE, y + 4, renderer.truncatedText(UI_10_FONT_ID, it.title.c_str(), pageWidth - 2 * SIDE - whenW).c_str(), !sel);
          // Second line: the rest of a long headline
          const std::string first1 = renderer.truncatedText(UI_10_FONT_ID, it.title.c_str(), pageWidth - 2 * SIDE - whenW);
          if (first1.size() >= 3 && first1.size() < it.title.size() + 3 && it.title.compare(0, first1.size() - 3, first1, 0, first1.size() - 3) == 0) {
            renderer.drawText(UI_10_FONT_ID, SIDE, y + 22, renderer.truncatedText(UI_10_FONT_ID, it.title.substr(first1.size() - 3).c_str(), pageWidth - 2 * SIDE).c_str(), !sel);
          }
          if (whenW) renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - whenW + 10, y + 6, it.when.c_str(), !sel);
        }
      }
      char pages[16];
      snprintf(pages, sizeof(pages), "%d/%d", count ? selected / itemsPerPage + 1 : 0, (count + itemsPerPage - 1) / itemsPerPage);
      renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom + 4, pages);
      renderer.drawText(SMALL_FONT_ID, SIDE, bottom + 4, tr(STR_NEWS_REFRESH_HINT));
      break;
    }
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_NEWS_LOADING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, failureDetail.c_str());
      break;
    case CONNECTING:
    case READING:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
