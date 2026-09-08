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
#include <cstring>

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
  if (bookDownloaded(book)) return true;
  const std::string path = cacheDir() + "/" + std::to_string(book) + "-" + std::to_string(chapter) + ".txt";
  return Storage.exists(path.c_str());
}

bool BibleActivity::readChapter(const int book, const int chapter, std::string& text) {
  // Primero el libro entero si está bajado; si no, el capítulo suelto cacheado.
  if (bookDownloaded(book) && readChapterFromBook(book, chapter, text)) return true;
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

// --- Biblia entera en la SD ------------------------------------------------
// Un archivo por libro, tal como lo manda el servidor: "#<capítulo>" y abajo los
// versículos numerados. Son 3,8 MB en español; con eso se lee y se busca sin
// WiFi (la voz igual necesita el servidor para pasar el audio a texto).

std::string BibleActivity::bookPath(const int book) const {
  char name[16];
  snprintf(name, sizeof(name), "/b%02d.txt", book);
  return cacheDir() + name;
}

bool BibleActivity::bookDownloaded(const int book) const { return Storage.exists(bookPath(book).c_str()); }

bool BibleActivity::bibleComplete() const {
  if (books.empty()) return false;
  for (size_t i = 0; i < books.size(); ++i) {
    if (!bookDownloaded(static_cast<int>(i))) return false;
  }
  return true;
}

// Saca un capítulo del archivo del libro. El libro más grande (Salmos) son
// 207 KB: entra en PSRAM sin problema.
bool BibleActivity::readChapterFromBook(const int book, const int chapter, std::string& text) const {
  HalFile f;
  if (!Storage.openFileForRead(TAG, bookPath(book), f)) return false;
  std::string whole;
  whole.resize(f.size());
  const int got = f.read(&whole[0], whole.size());
  f.close();
  if (got <= 0) return false;
  whole.resize(got);
  const std::string marker = "#" + std::to_string(chapter) + "\n";
  size_t start = whole.compare(0, marker.size(), marker) == 0 ? 0 : whole.find("\n" + marker);
  if (start == std::string::npos) return false;
  start += (start == 0 ? 0 : 1) + marker.size();
  size_t end = whole.find("\n#", start);
  if (end == std::string::npos) end = whole.size();
  text = whole.substr(start, end - start);
  return !text.empty();
}

bool BibleActivity::downloadBook(const int book) {
  ServerClient::Response resp;
  const ServerClient::Result r =
      SERVER_CLIENT.get("/api/bible/book?lang=" + lang + "&book=" + std::to_string(book), resp);
  if (r != ServerClient::Result::Ok || resp.body.size() < 32) {
    LOG_ERR(TAG, "libro %d: %s (%d)", book, ServerClient::resultName(r), resp.status);
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite(TAG, bookPath(book), f)) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
  f.close();
  downloadedKb += static_cast<int>(resp.body.size() / 1024);
  return written == resp.body.size();
}

namespace {
// Minúsculas y sin acentos, para comparar nombres y buscar texto.
std::string norm(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = in[i];
    if (c == 0xC3 && i + 1 < in.size()) {  // UTF-8 de las vocales acentuadas
      const unsigned char n = in[++i];
      const char* map = "aaaaaaaceeeeiiiidnooooo*ouuuuy";
      if (n >= 0x80 && n <= 0x9E) out += map[n - 0x80];
      else if (n >= 0xA0 && n <= 0xBE) out += map[n - 0xA0];
      continue;
    }
    if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += static_cast<char>(c);
    else if (c == ' ') out += ' ';
    else if (c < 0x80) out += ' ';
  }
  // Espacios de más
  std::string tidy;
  bool space = true;
  for (const char c : out) {
    if (c == ' ') {
      if (!space) tidy += c;
      space = true;
    } else {
      tidy += c;
      space = false;
    }
  }
  while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
  return tidy;
}
}  // namespace

// "juan 3 16", "salmo 23", "primera de juan 3" -> libro, capítulo, versículo.
bool BibleActivity::parseRefLocal(const std::string& spoken, int& book, int& chapter, int& verse) const {
  std::string s = norm(spoken);
  // Ordinales dichos con palabras
  const char* ord[][2] = {{"primera", "1"}, {"primero", "1"}, {"segunda", "2"}, {"segundo", "2"},
                          {"tercera", "3"}, {"tercero", "3"}, {"first", "1"},   {"second", "2"},
                          {"third", "3"}};
  for (const auto& o : ord) {
    const size_t at = s.find(o[0]);
    if (at != std::string::npos) s = s.substr(0, at) + o[1] + s.substr(at + strlen(o[0]));
  }
  // Números del final: capítulo y versículo
  int nums[2] = {0, 0};
  int found = 0;
  size_t end = s.size();
  while (found < 2 && end > 0) {
    size_t i = end;
    while (i > 0 && s[i - 1] == ' ') i--;
    size_t j = i;
    while (j > 0 && s[j - 1] >= '0' && s[j - 1] <= '9') j--;
    if (j == i) break;
    nums[found++] = atoi(s.substr(j, i - j).c_str());
    end = j;
  }
  if (!found) return false;
  std::string name = s.substr(0, end);
  while (!name.empty() && name.back() == ' ') name.pop_back();
  // "de", "del", "of": ruido en cualquier posición ("primera de juan" -> "1 juan").
  {
    const char* filler[] = {"de", "del", "of", "du", "des", "von", "da", "do"};
    std::string clean;
    size_t at = 0;
    while (at <= name.size()) {
      const size_t sp = name.find(' ', at);
      const std::string word = name.substr(at, (sp == std::string::npos ? name.size() : sp) - at);
      bool drop = false;
      for (const char* f : filler) drop = drop || word == f;
      if (!drop && !word.empty()) clean += (clean.empty() ? "" : " ") + word;
      if (sp == std::string::npos) break;
      at = sp + 1;
    }
    name = clean;
  }
  if (name.empty()) return false;
  int best = -1;
  for (size_t i = 0; i < books.size(); ++i) {
    const std::string candidate = norm(books[i].name);
    if (candidate == name) { best = static_cast<int>(i); break; }
    if (best < 0 && candidate.compare(0, name.size(), name) == 0) best = static_cast<int>(i);
  }
  if (best < 0) return false;
  book = best;
  chapter = found == 2 ? nums[1] : nums[0];
  verse = found == 2 ? nums[0] : 0;
  if (chapter < 1 || chapter > books[best].chapters) return false;
  return true;
}

// Un libro por pasada, para que la pantalla siga viva mientras busca.
void BibleActivity::searchStep() {
  if (searchIndex >= static_cast<int>(books.size()) || hits.size() >= 12) {
    finishSearch();
    return;
  }
  const int book = searchIndex++;
  HalFile f;
  if (Storage.openFileForRead(TAG, bookPath(book), f)) {
    std::string whole;
    whole.resize(f.size());
    const int got = f.read(&whole[0], whole.size());
    f.close();
    if (got > 0) {
      whole.resize(got);
      int chapter = 0;
      size_t pos = 0;
      while (pos < whole.size() && hits.size() < 12) {
        const size_t nl = whole.find('\n', pos);
        const std::string line = whole.substr(pos, (nl == std::string::npos ? whole.size() : nl) - pos);
        pos = nl == std::string::npos ? whole.size() : nl + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
          chapter = atoi(line.c_str() + 1);
          continue;
        }
        // Todas las palabras, en cualquier orden y no pegadas: "misericordia y
        // verdad" tiene que encontrar "La misericordia y la verdad se encontraron".
        const std::string flat = norm(line);
        bool all = true;
        for (const std::string& w : searchWords) {
          if (flat.find(w) == std::string::npos) {
            all = false;
            break;
          }
        }
        if (!all) continue;
        const size_t sp = line.find(' ');
        const int verse = atoi(line.c_str());
        hits.push_back({book, chapter, verse});
        std::string label = books[book].name + " " + std::to_string(chapter) + ":" + std::to_string(verse) + "  " +
                            (sp == std::string::npos ? line : line.substr(sp + 1));
        if (label.size() > 70) label = label.substr(0, 70) + "...";
        pickerOptions.push_back(label);
      }
    }
  }
  requestUpdate();
}

void BibleActivity::finishSearch() {
  if (hits.empty()) {
    fail(StrId::STR_BIBLE_NOT_FOUND, searchQuery);
    return;
  }
  state = PICK_RESULT;
  picker.show(StrId::STR_BIBLE_RESULTS, pickerOptions, 0, [this](int idx) {
    if (idx >= 0 && idx < static_cast<int>(hits.size())) {
      openChapter(hits[idx].book, hits[idx].chapter, hits[idx].verse);
    } else {
      state = BOOKS;
      requestUpdate();
    }
  });
  requestUpdate();
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
  // Con la Biblia entera en la tarjeta, la referencia y la búsqueda se resuelven
  // acá: el servidor solo hizo falta para pasar la voz a texto.
  if (bibleComplete()) {
    WiFi.setSleep(true);
    int b = 0, ch = 1, v = 0;
    if (parseRefLocal(spoken, b, ch, v)) {
      openChapter(b, ch, v);
      return;
    }
    searchQuery = norm(spoken);
    searchWords.clear();
    for (size_t at = 0; at <= searchQuery.size();) {
      const size_t sp = searchQuery.find(' ', at);
      const std::string w = searchQuery.substr(at, (sp == std::string::npos ? searchQuery.size() : sp) - at);
      if (w.size() > 2) searchWords.push_back(w);
      if (sp == std::string::npos) break;
      at = sp + 1;
    }
    if (searchWords.empty()) {
      fail(StrId::STR_BIBLE_NOT_FOUND, spoken);
      return;
    }
    hits.clear();
    pickerOptions.clear();
    searchIndex = 0;
    state = SEARCHING;
    requestUpdate();
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
      } else if (pending == DOWNLOAD) {
        pending = NONE;
        downloadIndex = 0;
        downloadedKb = 0;
        state = DOWNLOADING;
        requestUpdate();
      } else {
        state = stateAfterConnect;
        requestUpdate();
      }
      break;
    }
    case DOWNLOADING: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.setSleep(true);
        state = BOOKS;
        requestUpdate();
        break;
      }
      if (downloadIndex >= static_cast<int>(books.size())) {
        WiFi.setSleep(true);
        LOG_INF(TAG, "Biblia completa: %d KB", downloadedKb);
        state = BOOKS;
        requestUpdate();
        break;
      }
      const int book = downloadIndex++;
      if (!bookDownloaded(book) && !downloadBook(book)) {
        fail(StrId::STR_ASK_FAILED, "libro " + std::to_string(book));
        break;
      }
      requestUpdate();  // una pasada por libro: la pantalla sigue viva
      break;
    }
    case SEARCHING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        state = BOOKS;
        requestUpdate();
        break;
      }
      searchStep();
      break;
    case BOOKS:
    case CHAPTERS: {
      const bool inBooks = state == BOOKS;
      // La última fila de los libros es "descargar la Biblia entera".
      const int extra = inBooks ? 1 : 0;
      const int count = (inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters) + extra;
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
        if (inBooks && bookIndex == static_cast<int>(books.size())) {
          if (bibleComplete()) break;  // ya está toda
          pending = DOWNLOAD;
          ensureConnected(DOWNLOADING);
          break;
        }
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
      const int extra = inBooks ? 1 : 0;  // fila final: bajar la Biblia entera
      const int count = (inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters) + extra;
      const int selected = inBooks ? bookIndex : chapterIndex;
      const int top = metrics.topPadding + metrics.headerHeight + 10;
      const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
      itemsPerPage = std::max(1, (bottom - top) / ROW_H);
      const int first = (selected / itemsPerPage) * itemsPerPage;
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * ROW_H;
        const bool sel = i == selected;
        if (sel) renderer.fillRoundedRect(SIDE - 6, y, pageWidth - 2 * (SIDE - 6), ROW_H - 4, 8, Color::Black);
        const bool isDownloadRow = inBooks && i == static_cast<int>(books.size());
        std::string label = isDownloadRow ? std::string(bibleComplete() ? tr(STR_BIBLE_DOWNLOADED) : tr(STR_BIBLE_DOWNLOAD_ALL))
                            : inBooks     ? books[i].name
                                          : (tr(STR_BIBLE_CHAPTER) + std::string(" ") + std::to_string(i + 1));
        renderer.drawText(UI_12_FONT_ID, SIDE, y + 7, renderer.truncatedText(UI_12_FONT_ID, label.c_str(), pageWidth - 2 * SIDE - 40).c_str(), !sel);
        if (isDownloadRow) {
          // nada a la derecha
        } else if (inBooks) {
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
    case DOWNLOADING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, tr(STR_BIBLE_DOWNLOADING), true, EpdFontFamily::BOLD);
      char line[64];
      snprintf(line, sizeof(line), "%d / %d  ·  %d KB", downloadIndex, (int)books.size(), downloadedKb);
      renderer.drawCenteredText(UI_10_FONT_ID, mid, line);
      const int barW = pageWidth - 120;
      renderer.drawRect(60, mid + 30, barW, 14, true);
      if (!books.empty()) {
        const int fill = barW * downloadIndex / static_cast<int>(books.size());
        renderer.fillRect(62, mid + 32, std::max(2, fill - 4), 10, true);
      }
      confirmLabel = "";
      break;
    }
    case SEARCHING: {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 20, tr(STR_BIBLE_SEARCHING), true, EpdFontFamily::BOLD);
      char line[48];
      snprintf(line, sizeof(line), "%d / %d  ·  %d", searchIndex, (int)books.size(), (int)hits.size());
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 14, line);
      confirmLabel = "";
      break;
    }
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
