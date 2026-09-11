#include "NewsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ListStyle.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UrlEncode.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "NEWS";

constexpr const char* DIR = "/.crosspoint/rss";
constexpr const char* CACHE = "/.crosspoint/rss/feeds.json";
constexpr int SIDE = listui::SIDE;
constexpr int PAGER_H = 24;
constexpr unsigned long REFRESH_HOLD_MS = 1200;
// Un trozo de lectura: una frase entera que entre en la pantalla y que el
// servidor pueda sintetizar de una (su tope es 4000 caracteres).
// 700 caracteres son unos 45 s de voz: 350 KB de ADPCM y 1,4 MB de WAV ya
// decodificado, y de esos hay dos vivos a la vez (el que suena y el que se está
// bajando). Con 900 la PSRAM se ponía fea.
constexpr size_t CHUNK_MAX = 700;
constexpr size_t CHUNK_MIN = 250;
// La tarea de audio tarda un toque en arrancar: sin esta gracia, isPlaying()
// da false justo después de pedir la reproducción y el trozo se saltearía solo.
constexpr unsigned long SPEAK_GRACE_MS = 600;
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
  speech.stop();
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
  // `cache:false` quiere decir que ese texto NO es la nota: es la explicación
  // de por qué no se pudo traer ("el diario no contesta", "hay que
  // suscribirse"). Se muestra, pero no se guarda.
  //
  // Guardarlo era lo peor de los dos mundos: `openArticle()` prioriza el
  // archivo de la tarjeta, así que el mensaje de error quedaba pegado ahí para
  // siempre y la nota de verdad no se volvía a pedir nunca, ni cuando el diario
  // se recuperaba. Actualizar los titulares tampoco lo borraba.
  if (!(doc["cache"] | true)) {
    LOG_INF(TAG, "el servidor dice que esto no se cachea: se muestra pero no se guarda");
    return true;
  }
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
    // Rescate de lo que quedó envenenado antes del arreglo de fetchArticle: en
    // la tarjeta puede haber, guardado como si fuera la nota, el mensaje de
    // "no se pudo traer". No hay forma de distinguirlo con certeza de una nota
    // de verdad, pero un cuerpo de menos de RESCUE_MIN_CHARS no es una noticia
    // y sí tiene el tamaño exacto de esas explicaciones. Se intenta bajarla de
    // nuevo; si no hay red o el diario sigue sin contestar, se muestra lo
    // guardado igual, así el rescate nunca deja al usuario con menos que antes.
    if (text.size() >= RESCUE_MIN_CHARS) {
      showArticle(title, text);
      return;
    }
    LOG_INF(TAG, "artículo sospechosamente corto (%u bytes): se reintenta bajarlo", (unsigned)text.size());
    rescueTitle = title;
    rescueText = text;
  } else {
    rescueTitle.clear();
    rescueText.clear();
  }
  pending = ARTICLE_FETCH;
  ensureConnected();
}

void NewsActivity::showArticle(const std::string& title, const std::string& text) {
  articleTitle = title;
  articleText = text;
  chunks = textchunks::split(articleText, CHUNK_MAX, CHUNK_MIN);
  chunkIndex = 0;
  notice.clear();
  stopSpeaking();
  state = ARTICLE;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Leer la noticia en voz alta
// ---------------------------------------------------------------------------

std::string NewsActivity::chunkText(const int index) const {
  if (index < 0 || index >= static_cast<int>(chunks.size())) return "";
  return articleText.substr(chunks[index].start, chunks[index].len);
}

// GET /api/tts?text=&lang=&max= -> el clip ADPCM del trozo. `max` es el tope en
// segundos y por default son 20: sin mandarlo, cada trozo se cortaba ahí.
bool NewsActivity::fetchClip(const int index, std::string& out) {
  const std::string text = chunkText(index);
  if (text.empty()) return false;
  ServerClient::Response resp;
  const std::string path =
      std::string("/api/tts?lang=") + uiLanguageCode() + "&max=90&text=" + urlEncode(text);
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok || resp.body.size() < 16) {
    LOG_ERR(TAG, "GET /api/tts: %s %d (%u bytes)", ServerClient::resultName(r), resp.status,
            (unsigned)resp.body.size());
    return false;
  }
  out = std::move(resp.body);
  return true;
}

void NewsActivity::requestClip() {
  pending = CLIP;
  state = LOADING;
  requestUpdate();
}

// Se hace desde loop() (bloquea lo que tarde el servidor) con la pantalla ya
// pintada, como el resto de las llamadas de red del aparato.
void NewsActivity::serveClip() {
  WiFi.setSleep(false);
  bool ok = false;
  if (nextClipIndex == chunkIndex && !nextClip.empty()) {
    clip.swap(nextClip);  // ya estaba bajado mientras sonaba el anterior
    ok = true;
  } else {
    clip.clear();
    ok = fetchClip(chunkIndex, clip);
  }
  nextClip.clear();
  nextClipIndex = -1;
  state = ARTICLE;
  if (!ok) {
    stopSpeaking();
    notice = tr(STR_NEWS_SPEAK_FAILED);
    requestUpdate();
    return;
  }
  speaking = true;
  playClip();
  requestUpdate();
}

void NewsActivity::playClip() {
  paused = false;
  speakStartedAt = millis();
  if (!speech.playAdpcm(reinterpret_cast<const uint8_t*>(clip.data()), clip.size())) {
    stopSpeaking();
    notice = tr(STR_NEWS_SPEAK_FAILED);
  }
}

void NewsActivity::startSpeaking() {
  notice.clear();
  if (chunks.empty()) return;
  speaking = true;
  paused = false;
  nextClip.clear();
  nextClipIndex = -1;
  pending = CLIP;
  ensureConnected();  // con el WiFi ya arriba entra derecho a LOADING
}

void NewsActivity::stopSpeaking() {
  speech.stop();
  speaking = false;
  paused = false;
  clip.clear();
  nextClip.clear();
  nextClipIndex = -1;
  if (ServerClient::networkUp()) WiFi.setSleep(true);
}

// Mientras suena un trozo se baja el siguiente, así entre uno y otro no hay
// silencio; cuando termina, el que sigue arranca en el acto.
void NewsActivity::pumpSpeech() {
  if (!speaking || paused) return;
  const int next = chunkIndex + 1;
  if (speech.hasStarted() && nextClipIndex != next && next < static_cast<int>(chunks.size())) {
    nextClip.clear();
    nextClipIndex = next;  // se marca antes de pedirlo: si falla, no se reintenta en bucle
    if (!fetchClip(next, nextClip)) nextClip.clear();
  }
  if (millis() - speakStartedAt < SPEAK_GRACE_MS || speech.isPlaying()) return;
  if (next >= static_cast<int>(chunks.size())) {  // se terminó la noticia
    stopSpeaking();
    requestUpdate();
    return;
  }
  chunkIndex = next;
  if (nextClipIndex == chunkIndex && !nextClip.empty()) {
    speech.stop();
    clip.swap(nextClip);
    nextClip.clear();
    nextClipIndex = -1;
    playClip();
    requestUpdate();
    return;
  }
  requestClip();
}

// Arriba y abajo saltan de trozo. Si está leyendo, la lectura salta con el
// cursor (que es lo que uno espera al adelantar).
void NewsActivity::jumpChunk(const int delta) {
  const int count = static_cast<int>(chunks.size());
  if (count == 0) return;
  const int target = chunkIndex + delta;
  if (target < 0 || target >= count) return;
  chunkIndex = target;
  if (!speaking) {
    requestUpdate();
    return;
  }
  speech.stop();
  const bool haveIt = nextClipIndex == chunkIndex && !nextClip.empty();
  if (haveIt) {
    clip.swap(nextClip);
    nextClip.clear();
    nextClipIndex = -1;
    playClip();
    requestUpdate();
    return;
  }
  nextClip.clear();
  nextClipIndex = -1;
  requestClip();
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
    // Sin WiFi no hay voz, pero el artículo se sigue leyendo en pantalla: se
    // avisa ahí mismo en vez de tirar al usuario a la pantalla de error.
    if (pending == CLIP) {
      pending = NONE;
      stopSpeaking();
      notice = tr(STR_NEWS_SPEAK_NO_WIFI);
      state = ARTICLE;
      requestUpdate();
      return;
    }
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
      } else if (p == ARTICLE_FETCH) {
        std::string title, text;
        const bool ok = fetchArticle(feeds[feedIndex].id, feeds[feedIndex].items[itemIndex].id, title, text);
        WiFi.setSleep(true);
        if (!ok) {
          // Si veníamos de rescatar un artículo corto guardado, se muestra lo
          // que había: el rescate no puede dejar al usuario con menos de lo que
          // ya tenía en la mano.
          if (!rescueText.empty()) {
            LOG_INF(TAG, "no se pudo rebajar: se muestra lo guardado");
            showArticle(rescueTitle, rescueText);
            rescueTitle.clear();
            rescueText.clear();
            break;
          }
          // El detalle se pinta en pantalla: el titular del feed en vez de un "article" en inglés.
          fail(StrId::STR_ASK_FAILED, feeds[feedIndex].items[itemIndex].title);
          break;
        }
        rescueTitle.clear();
        rescueText.clear();
        showArticle(title, text);
      } else if (p == CLIP) {
        serveClip();
      } else {
        state = FEEDS;
        requestUpdate();
      }
      break;
    }
    case ARTICLE: {
      // Atrás corta la lectura y deja el artículo; otro Atrás vuelve a la lista.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        if (speaking) {
          stopSpeaking();
          requestUpdate();
        } else {
          state = ITEMS;
          requestUpdate();
        }
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (!speaking) {
          startSpeaking();
        } else if (paused) {
          // Reanuda donde iba. Si el clip ya se drenó mientras estaba en pausa
          // (la tarea de audio termina igual), se vuelve a poner el trozo.
          paused = false;
          if (speech.isPlaying()) speech.resume();
          else playClip();
          requestUpdate();
        } else {
          speech.pause();
          paused = true;
          requestUpdate();
        }
        break;
      }
      buttonNavigator.onNext([this] { jumpChunk(1); });
      buttonNavigator.onPrevious([this] { jumpChunk(-1); });
      if (state == ARTICLE) pumpSpeech();
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
      break;
  }
}

// El artículo, un trozo por pantalla: el mismo pedazo que se lee en voz alta,
// así lo que se escucha y lo que se ve van juntos.
void NewsActivity::renderArticle() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int top = metrics.topPadding + metrics.headerHeight + listui::GAP;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int total = static_cast<int>(chunks.size());
  const int w = pageWidth - 2 * SIDE;

  // Arriba queda sólo si está leyendo: por dónde va lo dice el paginador de
  // abajo, que es el mismo de todas las listas.
  if (speaking) {
    const char* label = paused ? tr(STR_NEWS_PAUSED) : tr(STR_NEWS_READING);
    renderer.drawText(UI_10_FONT_ID, pageWidth - SIDE - renderer.getTextWidth(UI_10_FONT_ID, label), top, label);
  }
  top += 26;

  if (!notice.empty()) {
    for (const std::string& line : renderer.wrappedText(UI_10_FONT_ID, notice.c_str(), w, 2)) {
      renderer.drawText(UI_10_FONT_ID, SIDE, top, line.c_str(), true, EpdFontFamily::BOLD);
      top += 22;
    }
    top += 6;
  }

  const int hintY = bottom - listui::HINT_H;
  const int pagerY = hintY - PAGER_H;
  const int room = pagerY - listui::GAP - top;
  const std::string text = chunkText(chunkIndex);

  // El cuerpo va en la cara de lectura y con el paso de renglón del visor
  // (40 px): en UI_10 pegado a 26 era la pantalla más incómoda de leer del
  // aparato. Pero acá el trozo ya está cortado por el servidor y NO se puede
  // perder el final, así que si no entra se aprieta el interlineado y recién
  // después se baja de cara. La cuenta se hace midiendo, no a ojo.
  const auto fits = [&](const int font, const int step) {
    const int lines = std::max(1, room / step);
    return static_cast<int>(renderer.wrappedText(font, text.c_str(), w, lines + 1).size()) <= lines;
  };
  int font = UI_12_FONT_ID;
  int step = std::max(40, renderer.getLineHeight(UI_12_FONT_ID));
  if (!fits(font, step)) step = renderer.getLineHeight(UI_12_FONT_ID) + 6;
  if (!fits(font, step)) {
    font = UI_10_FONT_ID;
    step = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  int y = top;
  for (const std::string& line : renderer.wrappedText(font, text.c_str(), w, std::max(1, room / step))) {
    renderer.drawText(font, SIDE, y, line.c_str());
    y += step;
  }

  listui::pager(renderer, SIDE, pagerY, w, total ? chunkIndex + 1 : 0, total, tr(STR_NEWS_PART_FORMAT));
  listui::hint(renderer, hintY, speaking ? tr(STR_NEWS_READING_HINT) : tr(STR_NEWS_ARTICLE_HINT));
}

void NewsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  const std::string title = state == ARTICLE  ? articleTitle
                            : state == ITEMS  ? feeds[feedIndex].name
                                              : std::string(tr(STR_HUB_NEWS));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const char* okLabel = tr(STR_SELECT);
  switch (state) {
    case ARTICLE:
      okLabel = !speaking ? tr(STR_NEWS_SPEAK) : paused ? tr(STR_NEWS_RESUME) : tr(STR_NEWS_PAUSE);
      renderArticle();
      break;
    case FEEDS:
    case ITEMS: {
      const bool inFeeds = state == FEEDS;
      const int count = inFeeds ? static_cast<int>(feeds.size()) : static_cast<int>(feeds[feedIndex].items.size());
      const int selected = inFeeds ? feedIndex : itemIndex;
      const int x = listui::SIDE;
      const int w = listui::contentWidth(renderer);
      const int top = listui::contentTop();
      const int hintY = listui::contentBottom(renderer) - listui::HINT_H;
      const int pagerY = hintY - PAGER_H;
      itemsPerPage = std::max(1, (pagerY - listui::GAP - top) / listui::ROW2_H);
      const int page = count > 0 ? selected / itemsPerPage : 0;
      const int first = page * itemsPerPage;
      if (count == 0) renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, inFeeds ? tr(STR_NEWS_NO_FEEDS) : tr(STR_NEWS_NO_ITEMS));
      for (int i = first; i < count && i < first + itemsPerPage; ++i) {
        const int y = top + (i - first) * listui::ROW2_H;
        listui::RowSpec spec;
        spec.selected = i == selected;
        std::string title;
        std::string detail;
        std::string meta;
        if (inFeeds) {
          const Feed& f = feeds[i];
          title = f.name;
          spec.bold = true;
          // El primer titular como vista previa: dice de qué va el feed sin
          // tener que entrar.
          if (!f.items.empty()) detail = f.items[0].title;
          meta = std::to_string(f.items.size());
        } else {
          const Item& it = feeds[feedIndex].items[i];
          meta = it.when;
          const int metaW = meta.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, meta.c_str()) + listui::META_GAP;
          title = renderer.truncatedText(UI_12_FONT_ID, it.title.c_str(), w - 2 * listui::PAD - metaW);
          // Segunda línea: sólo lo que quedó afuera del titular.
          detail = listui::tailAfterEllipsis(title, it.title);
        }
        spec.title = title.c_str();
        spec.detail = detail.empty() ? nullptr : detail.c_str();
        spec.meta = meta.empty() ? nullptr : meta.c_str();
        listui::row(renderer, x, y, w, listui::ROW2_H, spec);
      }
      listui::pager(renderer, x, pagerY, w, page + 1, count > 0 ? (count + itemsPerPage - 1) / itemsPerPage : 1);
      listui::hint(renderer, hintY, tr(STR_NEWS_REFRESH_HINT));
      break;
    }
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, speaking ? tr(STR_NEWS_SPEAK_LOADING) : tr(STR_NEWS_LOADING),
                                true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, mid + 10,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    case CONNECTING:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), okLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de refrescos limpios la lleva el coordinador del panel.
  renderer.displayBuffer();
}
