#include "BibleActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include <algorithm>

#include "HubStore.h"
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/home/AssetSyncActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"
#include "voice/VoiceNotes.h"  // mmss(): el contador de la grabación
#include "activities/ListStyle.h"
#include "components/Selection.h"

namespace {
constexpr const char* TAG = "BIBLE";
constexpr int PAGER_H = 24;  // franja del paginador, debajo de las filas
constexpr unsigned long VOICE_HOLD_MS = 1200;
constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel: refresco limpio cada 10-15 parciales
constexpr uint32_t ASK_TIMEOUT_MS = 90000;   // transcripción + LLM del lado del servidor
constexpr size_t MAX_CHAPTER_BYTES = 24 * 1024;  // lo que se manda del capítulo (Salmo 119 es el único que roza esto)
constexpr uint32_t RECORD_SECONDS = 20;
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
    refreshCardCount();
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
  // Sin la Biblia entera en la tarjeta, casi todo lo que falla se arregla
  // bajando el paquete de contenido: se ofrece ir ahí en vez del error pelado.
  // Lo que falla al preguntar no: eso es el servidor o la red, y bajar la
  // Biblia no lo arregla.
  offerAssets = !suppressAssetOffer && !bibleComplete();
  suppressAssetOffer = false;
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
// Un archivo por libro, tal como lo deja el paquete de contenido: "#<capítulo>"
// y abajo los versículos numerados. Son 3,8 MB en español; con eso se lee y se
// busca sin WiFi (la voz igual necesita el servidor para pasar el audio a
// texto). La descarga la hace `AssetSyncActivity`, no esta pantalla.

std::string BibleActivity::bookPath(const int book) const {
  char name[16];
  snprintf(name, sizeof(name), "/b%02d.txt", book);
  return cacheDir() + name;
}

bool BibleActivity::bookDownloaded(const int book) const { return Storage.exists(bookPath(book).c_str()); }

// Con el recuento en memoria: mirar 66 archivos de la SD en cada dibujo de la
// lista era carísimo.
bool BibleActivity::bibleComplete() const { return !books.empty() && booksOnCard >= static_cast<int>(books.size()); }

void BibleActivity::refreshCardCount() {
  booksOnCard = 0;
  for (size_t i = 0; i < books.size(); ++i) {
    if (bookDownloaded(static_cast<int>(i))) booksOnCard++;
  }
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
  asking = false;  // lo que venga a continuación es traer texto, no una pregunta
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
  // El capítulo va al visor de lectura con la referencia de título (UI_14) y en
  // modo versículos: el número que abre cada uno sale en SMALL negrita, así se
  // sigue una cita sin que un número del tamaño del texto corte la lectura.
  auto viewer = std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, title, body,
                                                               /*htmlDefinition=*/false,
                                                               /*verseNumbers=*/true);
  // "A la Biblia no le encuentro el comando para preguntarle cosas": preguntar
  // solo existía en la lista de capítulos, y uno pregunta mientras LEE. Atrás
  // mantenido dentro del capítulo abre el menú de voz con ese capítulo cargado,
  // y la barra de abajo lo dice.
  viewer->setVoiceHold(tr(STR_BIBLE_HOLD_ASK), [this] { askAfterViewer = true; });
  startActivityForResult(std::move(viewer), [this](const ActivityResult&) {
    state = CHAPTERS;
    if (askAfterViewer) {
      askAfterViewer = false;
      openVoiceMenu();
      return;
    }
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
    // Leer y buscar andan sin conexión con la Biblia en la tarjeta; preguntar
    // no, y decirlo así es más útil que "falló el WiFi".
    if (pending == ASK) {
      pending = NONE;
      recorder.abort();
      suppressAssetOffer = true;
      fail(StrId::STR_BIBLE_ASK_NEEDS_WIFI);
      return;
    }
    fail(StrId::STR_SERVER_WIFI_FAILED);
    return;
  }
  state = LOADING;  // the request runs from loop() so the screen paints first
  requestUpdate();
}

// En la lista de capítulos el botón de voz hace dos cosas distintas, así que
// primero se pregunta cuál: sin esto no hay forma de que se vea que además de
// buscar se puede preguntar (mantener OK no sirve, en esta placa apaga).
void BibleActivity::openVoiceMenu() {
  menuOptions = {tr(STR_BIBLE_MENU_SEARCH), tr(STR_BIBLE_MENU_ASK)};
  state = MENU;
  picker.show(StrId::STR_BIBLE_ASK_TITLE, menuOptions, 0, [this](const int idx) { startVoice(idx == 1); });
  requestUpdate();
}

void BibleActivity::startVoice(const bool ask) {
  asking = ask;
  if (!SERVER_STORE.hasToken()) {
    suppressAssetOffer = ask;
    fail(StrId::STR_ASK_NO_TOKEN);
    return;
  }
  StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
  if (!recorder.start(why)) {
    suppressAssetOffer = ask;
    fail(why);
    return;
  }
  shownSecond = -1;
  forceClean = true;
  state = RECORDING;
  requestUpdate();
}

// El capítulo entero va en el cuerpo: el aparato ya lo tiene en la tarjeta y el
// servidor no guarda la Biblia por idioma del usuario.
void BibleActivity::performAsk() {
  std::string question, detail;
  const bool ok = SpeechToText::transcribe(recorder, question, detail);
  recorder.release();
  if (!ok) {
    suppressAssetOffer = true;
    fail(StrId::STR_ASK_TRANSCRIBE_FAILED, detail);
    return;
  }
  std::string chapter;
  if (!readChapter(bookIndex, chapterIndex + 1, chapter) && !fetchChapter(bookIndex, chapterIndex + 1, chapter)) {
    fail(StrId::STR_BIBLE_CHAPTER_FAILED);
    return;
  }
  if (chapter.size() > MAX_CHAPTER_BYTES) {
    LOG_INF(TAG, "capítulo de %u bytes recortado a %u", (unsigned)chapter.size(), (unsigned)MAX_CHAPTER_BYTES);
    chapter.resize(MAX_CHAPTER_BYTES);
  }
  std::string body;
  {
    JsonDocument doc;
    doc["book"] = books[bookIndex].name;
    doc["chapter"] = chapterIndex + 1;
    doc["text"] = chapter;
    doc["question"] = question;
    doc["lang"] = lang;
    serializeJson(doc, body);
  }
  chapter.clear();
  chapter.shrink_to_fit();
  LOG_INF(TAG, "POST /api/bible/ask: %u bytes (\"%s\")", (unsigned)body.size(), question.c_str());
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/bible/ask", body, resp, ASK_TIMEOUT_MS);
  body.clear();
  body.shrink_to_fit();
  WiFi.setSleep(true);
  suppressAssetOffer = true;
  if (r != ServerClient::Result::Ok) {
    char why[96];
    snprintf(why, sizeof(why), "%s (%d)", ServerClient::resultName(r), resp.status);
    fail(StrId::STR_ASK_FAILED, why);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    fail(StrId::STR_ASK_FAILED, tr(STR_VOICE_BAD_REPLY));
    return;
  }
  const std::string answer = doc["answer"] | "";
  if (answer.empty()) {
    fail(StrId::STR_ASK_FAILED, doc["error"] | tr(STR_VOICE_EMPTY_REPLY));
    return;
  }
  suppressAssetOffer = false;
  state = READING;
  // La pregunta transcripta como título, igual que en "Preguntarle al libro".
  startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, question, answer),
                         [this](const ActivityResult&) {
                           state = CHAPTERS;
                           forceClean = true;
                           requestUpdate();
                         });
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
      fail(StrId::STR_BIBLE_BOOKS_FAILED);
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
        fail(StrId::STR_BIBLE_BOOKS_FAILED);
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
          fail(StrId::STR_BIBLE_BOOKS_FAILED);
          break;
        }
        WiFi.setSleep(true);
        state = BOOKS;
        bookIndex = HUB_STORE.bibleBook < static_cast<int>(books.size()) ? HUB_STORE.bibleBook : 0;
        refreshCardCount();
        requestUpdate();
      } else if (pending == LOAD_CHAPTER) {
        pending = NONE;
        std::string text;
        if (!fetchChapter(bookIndex, chapterIndex + 1, text)) {
          fail(StrId::STR_BIBLE_CHAPTER_FAILED);
          break;
        }
        WiFi.setSleep(true);
        showChapter(text);
      } else if (pending == VOICE) {
        pending = NONE;
        performVoice();
      } else if (pending == ASK) {
        pending = NONE;
        performAsk();
      } else {
        state = stateAfterConnect;
        requestUpdate();
      }
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
      const int count = inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters;
      int& index = inBooks ? bookIndex : chapterIndex;
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, VOICE_HOLD_MS)) {
        // En los libros solo se puede buscar; en los capítulos hay uno elegido,
        // así que además se puede preguntar sobre él y hay que ofrecer las dos.
        if (inBooks) startVoice(/*ask=*/false);
        else openVoiceMenu();
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
          listTop = 0;
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
          listTop = 0;
          requestUpdate();
        }
      }
      break;
    }
    case RECORDING: {
      const State back = asking ? CHAPTERS : BOOKS;
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        state = back;
        forceClean = true;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        recorder.stop();
        forceClean = true;
        if (recorder.tooShort()) {
          state = back;
          requestUpdate();
          break;
        }
        pending = asking ? ASK : VOICE;
        ensureConnected(back);
        break;
      }
      if (!recorder.pump()) {
        suppressAssetOffer = true;
        fail(StrId::STR_AUDIO_CAPTURE_FAILED);
        break;
      }
      // El contador de segundos: se repinta segundo a segundo al principio y al
      // final, y de a cinco en el medio (cada repintado es un refresco del papel).
      const int seconds = static_cast<int>(recorder.seconds());
      const int left = static_cast<int>(RECORD_SECONDS) - seconds;
      if (seconds != shownSecond && (seconds <= 5 || left <= 5 || seconds % 5 == 0)) {
        shownSecond = seconds;
        requestUpdate();
      }
      break;
    }
    case MENU:
      if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
        if (state == MENU && !picker.isActive()) {  // Atrás en el menú: vuelve a los capítulos
          state = CHAPTERS;
          requestUpdate();
        }
      }
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
      // OK baja el paquete de contenido (ahí viene la Biblia entera); Atrás
      // vuelve a la lista.
      if (offerAssets && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        offerAssets = false;
        startActivityForResult(std::make_unique<AssetSyncActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 refreshCardCount();
                                 state = books.empty() ? FAILED : BOOKS;
                                 requestUpdate();
                               });
        break;
      }
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
      // Las dos listas de la Biblia con la fila común de todas las nuestras:
      // margen de 24, fila de 48 y el paginador con su frase entera abajo. El
      // "1/66" en la esquina, que era lo que había, no lo entendía nadie.
      const bool inBooks = state == BOOKS;
      const int count = inBooks ? static_cast<int>(books.size()) : books[bookIndex].chapters;
      const int selected = inBooks ? bookIndex : chapterIndex;
      const int x = listui::SIDE;
      const int w = listui::contentWidth(renderer);
      const int top = listui::contentTop();
      // Sin la Biblia entera en la tarjeta, abajo va el aviso de que viene en el
      // paquete de contenido: hay que dejarle un renglón.
      const bool notice = inBooks && !bibleComplete();
      const int noticeY = listui::contentBottom(renderer) - listui::HINT_H;
      const int hintY = notice ? noticeY - listui::HINT_H : noticeY;
      const int pagerY = hintY - PAGER_H;
      const int bottom = pagerY - listui::GAP;
      const int perPage = std::max(1, (bottom - top) / listui::ROW1_H);
      if (listTop > selected) listTop = selected;
      if (selected >= listTop + perPage) listTop = selected - perPage + 1;
      if (listTop < 0 || listTop >= count) listTop = 0;

      int y = top;
      for (int i = listTop; i < count && y + listui::ROW1_H <= bottom; ++i) {
        const int book = inBooks ? i : bookIndex;
        const std::string label = inBooks ? books[book].name
                                          : (tr(STR_BIBLE_CHAPTER) + std::string(" ") + std::to_string(i + 1));
        // En los capítulos el metadato dice si se lee sin WiFi; antes era un
        // cuadradito de 6 px que no se entendía sin manual.
        const std::string meta = inBooks ? std::to_string(books[book].chapters)
                                         : (chapterCached(bookIndex, i + 1) ? std::string(tr(STR_PHOTO_ON_CARD))
                                                                            : std::string());
        listui::RowSpec spec;
        spec.title = label.c_str();
        spec.meta = meta.empty() ? nullptr : meta.c_str();
        spec.selected = i == selected;
        listui::row(renderer, x, y, w, listui::ROW1_H, spec);
        y += listui::ROW1_H;
      }

      listui::pager(renderer, x, pagerY, w, selected / perPage + 1, (count + perPage - 1) / perPage);
      listui::hint(renderer, hintY, inBooks ? tr(STR_BIBLE_VOICE_HINT) : tr(STR_BIBLE_CHAPTER_HINT));
      if (notice) {
        // La Biblia entera ya no se baja desde acá: viene en el paquete.
        std::string line = tr(STR_BIBLE_FROM_PACKAGE);
        if (booksOnCard > 0) {
          line = std::to_string(booksOnCard) + "/" + std::to_string(books.size()) + "  ·  " + line;
        }
        renderer.drawCenteredText(SMALL_FONT_ID, noticeY,
                                  renderer.truncatedText(SMALL_FONT_ID, line.c_str(), w).c_str());
      }
      break;
    }
    case MENU:
      if (picker.processRender(renderer, mappedInput)) return;
      break;
    case RECORDING: {
      renderer.drawCenteredText(
          UI_12_FONT_ID, mid - 70,
          renderer
              .truncatedText(UI_12_FONT_ID, asking ? tr(STR_BIBLE_ASK_PROMPT) : tr(STR_BIBLE_VOICE_PROMPT),
                             pageWidth - 40, EpdFontFamily::BOLD)
              .c_str(),
          true, EpdFontFamily::BOLD);
      if (asking) {
        // Sobre qué se está preguntando, para que no haya dudas.
        const std::string ref = books[bookIndex].name + " " + std::to_string(chapterIndex + 1);
        renderer.drawCenteredText(UI_10_FONT_ID, mid - 36, ref.c_str());
      }
      // Los segundos que van y el tope: sin esto no hay forma de saber cuánto
      // se puede hablar.
      const int seconds = static_cast<int>(recorder.seconds());
      const std::string counter = std::string(tr(STR_REC_ELAPSED)) + "   " + voicenotes::mmss(seconds) + " / " +
                                  voicenotes::mmss(static_cast<int>(RECORD_SECONDS));
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 4, counter.c_str(), true, EpdFontFamily::BOLD);
      int exampleY = mid + 40;
      for (const std::string& line : renderer.wrappedText(
               UI_10_FONT_ID, asking ? tr(STR_BIBLE_ASK_EXAMPLES) : tr(STR_BIBLE_VOICE_EXAMPLES), pageWidth - 60, 2)) {
        renderer.drawCenteredText(UI_10_FONT_ID, exampleY, line.c_str());
        exampleY += 26;
      }
      break;
    }
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, asking ? tr(STR_ASK_ASKING) : tr(STR_BIBLE_LOADING),
                                true, EpdFontFamily::BOLD);
      confirmLabel = "";
      break;
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
      if (offerAssets) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 44, tr(STR_BIBLE_FROM_PACKAGE));
        confirmLabel = tr(STR_ASSETS_GET);
      }
      break;
    case CONNECTING:
    case READING:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Regla del panel: refresco limpio cada 10-15 parciales o la pantalla
  // fantasmea. Con el micrófono abierto se evita (medio segundo de SPI ahí come
  // muestras) y en su lugar se pide uno al entrar y otro al salir.
  const bool clean = forceClean || (state != RECORDING && ++partialCount >= PARTIALS_BEFORE_CLEAN);
  if (clean) {
    partialCount = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
