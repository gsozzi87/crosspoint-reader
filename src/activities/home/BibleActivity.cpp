#include "BibleActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "BIBLE";
constexpr int ROW_H = 40;
constexpr int SIDE = 20;
constexpr unsigned long VOICE_HOLD_MS = 1200;
}  // namespace

std::string BibleActivity::cacheDir() const { return std::string("/.crosspoint/bible/") + lang; }

void BibleActivity::onEnter() {
  Activity::onEnter();
  lang = uiLanguageCode();
  Storage.ensureDirectoryExists("/.crosspoint/bible");
  Storage.ensureDirectoryExists(cacheDir().c_str());
  if (loadBooksFromCache()) {
    state = BOOKS;
    bookIndex = HUB_STORE.bibleBook < static_cast<int>(books.size()) ? HUB_STORE.bibleBook : 0;
    requestUpdate();
  } else {
    ensureConnected(BOOKS);
    pending = LOAD_BOOKS;
  }
}

void BibleActivity::onExit() {
  Activity::onExit();
  recorder.abort();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void BibleActivity::fail(StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  recorder.abort();
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

bool BibleActivity::loadBooksFromCache() {
  const std::string path = cacheDir() + "/books.json";
  if (!Storage.exists(path.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  books.clear();
  for (JsonVariantConst b : doc["books"].as<JsonArrayConst>()) books.push_back({b["name"] | "", b["chapters"] | 0});
  return books.size() == 66;
}

bool BibleActivity::fetchBooks() {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/bible/books?lang=" + lang, resp);
  if (r != ServerClient::Result::Ok) return false;
  HalFile f;
  if (Storage.openFileForWrite(TAG, cacheDir() + "/books.json", f)) {
    f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
    f.close();
  }
  return loadBooksFromCache();
}

bool BibleActivity::chapterCached(const int book, const int chapter) const {
  const std::string path = cacheDir() + "/" + std::to_string(book) + "-" + std::to_string(chapter) + ".txt";
  return Storage.exists(path.c_str());
}

bool BibleActivity::readChapter(const int book, const int chapter, std::string& text) {
  const std::string path = cacheDir() + "/" + std::to_string(book) + "-" + std::to_string(chapter) + ".txt";
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  text.resize(f.size());
  const int got = f.read(&text[0], text.size());
  f.close();
  return got > 0;
}

bool BibleActivity::fetchChapter(const int book, const int chapter, std::string& text) {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get(
      "/api/bible/chapter?lang=" + lang + "&book=" + std::to_string(book) + "&chapter=" + std::to_string(chapter), resp);
  if (r != ServerClient::Result::Ok) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  text = doc["text"] | "";
  if (text.empty()) return false;
  HalFile f;
  if (Storage.openFileForWrite(TAG, cacheDir() + "/" + std::to_string(book) + "-" + std::to_string(chapter) + ".txt", f)) {
    f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    f.close();
  }
  return true;
}

void BibleActivity::saveLastRef() {
  HUB_STORE.bibleBook = bookIndex;
  HUB_STORE.bibleChapter = chapterIndex + 1;
  HUB_STORE.saveToFile();
}

void BibleActivity::openChapter(const int book, const int chapter, const int verse) {
  bookIndex = book;
  chapterIndex = chapter - 1;
  wantedVerse = verse;
  std::string text;
  if (readChapter(book, chapter, text)) {
    showChapter(text);
    return;
  }
  pending = LOAD_CHAPTER;
  ensureConnected(READING);
}

void BibleActivity::showChapter(const std::string& text) {
  saveLastRef();
  std::string body = text;
  if (wantedVerse > 1) {
    // Start the page at the asked verse ("16 ..."), the rest of the chapter follows.
    const std::string marker = "\n" + std::to_string(wantedVerse) + " ";
    const size_t at = body.find(marker);
    if (at != std::string::npos) body = body.substr(at + 1);
  }
  state = READING;
  const std::string title = books[bookIndex].name + " " + std::to_string(chapterIndex + 1);
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, title, body),
                         [this](const ActivityResult&) {
                           state = CHAPTERS;
                           requestUpdate();
                         });
}

void BibleActivity::ensureConnected(const State next) {
  stateAfterConnect = next;
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BibleActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = LOADING;  // the request runs from loop() so the screen paints first
  requestUpdate();
}

void BibleActivity::startVoice() {
  if (!SERVER_STORE.hasToken()) {
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    fail(why);
    return;
  }
  state = RECORDING;
  requestUpdate();
}

// Transcribe, then GET /api/bible/find: a reference opens straight away, a
// search shows the verses to pick from.
void BibleActivity::performVoice() {
  std::string spoken, detail;
  const bool ok = SpeechToText::transcribe(recorder, spoken, detail);
  recorder.release();
  if (!ok) {
    fail(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/bible/find?lang=" + lang + "&q=" + urlEncode(spoken), resp);
  WiFi.setSleep(true);
  JsonDocument doc;
  if (r != ServerClient::Result::Ok || deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    fail(StrId::STR_BIBLE_NOT_FOUND, spoken);
    return;
  }
  const std::string kind = doc["kind"] | "";
  if (kind == "ref") {
    if (books.empty() && !fetchBooks()) {
      fail(StrId::STR_ASK_FAILED, "books");
      return;
    }
    openChapter(doc["book"] | 0, doc["chapter"] | 1, doc["verse"] | 0);
    return;
  }
  hits.clear();
  pickerOptions.clear();
  for (JsonVariantConst h : doc["results"].as<JsonArrayConst>()) {
    hits.push_back({h["book"] | 0, h["chapter"] | 1, h["verse"] | 1});
    std::string line = std::string(h["name"] | "") + " " + std::to_string(h["chapter"] | 1) + ":" +
                       std::to_string(h["verse"] | 1) + "  " + (h["text"] | "");
    if (line.size() > 70) line = line.substr(0, 70) + "...";
    pickerOptions.push_back(line);
  }
  if (hits.empty()) {
    fail(StrId::STR_BIBLE_NOT_FOUND, spoken);
    return;
  }
  state = PICK_RESULT;
  picker.show(StrId::STR_BIBLE_RESULTS, pickerOptions, 0, [this](int idx) {
    if (idx >= 0 && idx < static_cast<int>(hits.size())) {
      if (books.empty() && !fetchBooks()) {
        fail(StrId::STR_ASK_FAILED, "books");
        return;
      }
      openChapter(hits[idx].book, hits[idx].chapter, hits[idx].verse);
    } else {
      state = BOOKS;
      requestUpdate();
    }
  });
  requestUpdate();
}

void BibleActivity::loop() {
  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      if (pending == LOAD_BOOKS) {
        pending = NONE;
        if (!fetchBooks()) {
          fail(StrId::STR_ASK_FAILED, "books");
          break;
        }
        WiFi.setSleep(true);
        state = BOOKS;
        bookIndex = HUB_STORE.bibleBook < static_cast<int>(books.size()) ? HUB_STORE.bibleBook : 0;
        requestUpdate();
      } else if (pending == LOAD_CHAPTER) {
        pending = NONE;
        std::string text;
        if (!fetchChapter(bookIndex, chapterIndex + 1, text)) {
          fail(StrId::STR_ASK_FAILED, "chapter");
          break;
        }
        WiFi.setSleep(true);
        showChapter(text);
      } else if (pending == VOICE) {
        pending = NONE;
        performVoice();
      } else {
        state = stateAfterConnect;
        requestUpdate();
      }
      break;
    }
    case BOOKS:
    case CHAPTERS: {
      const bool inBooks = state == BOOKS;
      const int count = inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters;
      int& index = inBooks ? bookIndex : chapterIndex;
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, VOICE_HOLD_MS)) {
        startVoice();
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
        if (inBooks) {
          state = CHAPTERS;
          chapterIndex = HUB_STORE.bibleBook == bookIndex && HUB_STORE.bibleChapter > 0 ? HUB_STORE.bibleChapter - 1 : 0;
          if (chapterIndex >= books[bookIndex].chapters) chapterIndex = 0;
          requestUpdate();
        } else {
          openChapter(bookIndex, chapterIndex + 1, 0);
        }
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        if (inBooks) {
          activityManager.goHome();
        } else {
          state = BOOKS;
          requestUpdate();
        }
      }
      break;
    }
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        state = BOOKS;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        recorder.stop();
        if (recorder.tooShort()) {
          state = BOOKS;
          requestUpdate();
          break;
        }
        pending = VOICE;
        ensureConnected(BOOKS);
        break;
      }
      if (!recorder.pump()) fail(StrId::STR_AUDIO_CAPTURE_FAILED);
      break;
    case PICK_RESULT:
      if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
        if (state == PICK_RESULT && !picker.isActive()) {
          state = BOOKS;
          requestUpdate();
        }
      }
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (books.empty()) {
          activityManager.goHome();
        } else {
          state = BOOKS;
          requestUpdate();
        }
      }
      break;
    case CONNECTING:
    case READING:
      break;
  }
}

void BibleActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  const std::string title = state == CHAPTERS ? books[bookIndex].name : std::string(tr(STR_HUB_BIBLE));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());
  const char* confirmLabel = tr(STR_SELECT);

  switch (state) {
    case BOOKS:
    case CHAPTERS: {
      const bool inBooks = state == BOOKS;
      const int count = inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters;
      const int selected = inBooks ? bookIndex : chapterIndex;
      const int top = metrics.topPadding + metrics.headerHeight + 10;
      const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
      itemsPerPage = std::max(1, (bottom - top) / ROW_H);
      const int first = (selected / itemsPerPage) * itemsPerPage;
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * ROW_H;
        const bool sel = i == selected;
        if (sel) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
        std::string label = inBooks ? books[i].name : (tr(STR_BIBLE_CHAPTER) + std::string(" ") + std::to_string(i + 1));
        renderer.drawText(UI_12_FONT_ID, SIDE, y + 7, renderer.truncatedText(UI_12_FONT_ID, label.c_str(), pageWidth - 2 * SIDE - 40).c_str(), !sel);
        if (inBooks) {
          const std::string n = std::to_string(books[i].chapters);
          renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(UI_10_FONT_ID, n.c_str()), y + 10, n.c_str(), !sel);
        } else if (chapterCached(bookIndex, i + 1)) {
          renderer.fillRect(pageWidth - SIDE - 6, y + ROW_H / 2 - 5, 6, 6);  // cached: readable offline
        }
      }
      char pages[16];
      snprintf(pages, sizeof(pages), "%d/%d", selected / itemsPerPage + 1, (count + itemsPerPage - 1) / itemsPerPage);
      renderer.drawText(SMALL_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(SMALL_FONT_ID, pages), bottom + 4, pages);
      renderer.drawText(SMALL_FONT_ID, SIDE, bottom + 4, tr(STR_BIBLE_VOICE_HINT));
      break;
    }
    case RECORDING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_BIBLE_VOICE_PROMPT), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, tr(STR_BIBLE_VOICE_EXAMPLES));
      break;
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_BIBLE_LOADING), true, EpdFontFamily::BOLD);
      confirmLabel = "";
      break;
    case PICK_RESULT:
      if (picker.processRender(renderer, mappedInput)) return;
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
    case READING:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
