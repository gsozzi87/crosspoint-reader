#include "CardsActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/home/AssetSyncActivity.h"
#include "components/UITheme.h"
#include "util/GrayText.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "CARDS";
// Dónde deja el paquete el índice de las tarjetas. Se prueban las dos rutas
// razonables por si el servidor lo cuelga de otro lado.
const char* const INDEX_PATHS[] = {
    "/.crosspoint/cards/index.json",
    "/.crosspoint/assets/cards/index.json",
};
constexpr int IMAGE_BOX = 360;  // el dibujo entra en un cuadrado de este lado

// ---------------------------------------------------------------------------
// Lector del índice (SAX, igual que el manifiesto del paquete: son cientos de
// tarjetas y no hace falta armar el documento entero en memoria).
// ---------------------------------------------------------------------------
struct IndexSink {
  std::vector<CardsActivity::Card>* out = nullptr;
  int depth = 0;
  bool inCards = false;
  std::string key;
  CardsActivity::Card cur;
};

IndexSink* sink(void* ctx) { return static_cast<IndexSink*>(ctx); }

bool keyIs(const std::string& key, const char* const* names, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (key == names[i]) return true;
  }
  return false;
}

void cOnKey(void* ctx, const char* key, const size_t len) { sink(ctx)->key.assign(key, len); }

void cOnString(void* ctx, const char* value, const size_t len) {
  IndexSink* s = sink(ctx);
  if (s->inCards && s->depth == 2) {
    // Nombres alternativos: el índice lo genera el servidor y más vale aceptar
    // las formas obvias que romperse por una letra.
    static const char* const ES[] = {"es", "word_es", "wordEs", "spanish", "espanol"};
    static const char* const EN[] = {"en", "word_en", "wordEn", "english", "ingles"};
    static const char* const IMG[] = {"img", "image", "bmp", "picture"};
    static const char* const AES[] = {"audioEs", "audio_es", "es_audio", "audioes"};
    static const char* const AEN[] = {"audioEn", "audio_en", "en_audio", "audioen"};
    static const char* const CAT[] = {"cat", "category", "categoria", "grupo"};
    const std::string v(value, len);
    if (s->key == "id") s->cur.id = v;
    else if (keyIs(s->key, ES, sizeof(ES) / sizeof(ES[0]))) s->cur.es = v;
    else if (keyIs(s->key, EN, sizeof(EN) / sizeof(EN[0]))) s->cur.en = v;
    else if (keyIs(s->key, IMG, sizeof(IMG) / sizeof(IMG[0]))) s->cur.img = v;
    else if (keyIs(s->key, AES, sizeof(AES) / sizeof(AES[0]))) s->cur.audioEs = v;
    else if (keyIs(s->key, AEN, sizeof(AEN) / sizeof(AEN[0]))) s->cur.audioEn = v;
    else if (keyIs(s->key, CAT, sizeof(CAT) / sizeof(CAT[0]))) s->cur.cat = v;
  }
  s->key.clear();
}

void cOnNumber(void* ctx, const char*, size_t) { sink(ctx)->key.clear(); }
void cOnBool(void*, bool) {}
void cOnNull(void*) {}

void cOnObjectStart(void* ctx) {
  IndexSink* s = sink(ctx);
  s->depth++;
  if (s->inCards && s->depth == 2) s->cur = CardsActivity::Card();
  s->key.clear();
}

void cOnObjectEnd(void* ctx) {
  IndexSink* s = sink(ctx);
  if (s->inCards && s->depth == 2 && !s->cur.id.empty() && (!s->cur.es.empty() || !s->cur.en.empty())) {
    s->out->push_back(s->cur);
  }
  s->depth--;
  s->key.clear();
}

void cOnArrayStart(void* ctx) {
  IndexSink* s = sink(ctx);
  if (s->depth == 1 && (s->key == "cards" || s->key == "items")) s->inCards = true;
  s->key.clear();
}

void cOnArrayEnd(void* ctx) {
  IndexSink* s = sink(ctx);
  s->inCards = false;
  s->key.clear();
}
}  // namespace

std::string CardsActivity::sdPath(const std::string& path) {
  if (path.empty()) return path;
  if (path[0] == '/') return path;
  return std::string("/.crosspoint/") + path;
}

// ---------------------------------------------------------------------------
// Carga
// ---------------------------------------------------------------------------

bool CardsActivity::loadIndex() {
  std::string raw;
  for (const char* path : INDEX_PATHS) {
    HalFile f;
    if (!Storage.openFileForRead(TAG, path, f)) continue;
    raw.resize(f.size());
    const int got = raw.empty() ? 0 : f.read(&raw[0], raw.size());
    f.close();
    if (got > 0) {
      raw.resize(static_cast<size_t>(got));
      break;
    }
    raw.clear();
  }
  if (raw.empty()) return false;

  cards.clear();
  IndexSink handler;
  handler.out = &cards;
  StreamingJsonParser parser(JsonCallbacks{&handler, cOnKey, cOnString, cOnNumber, cOnBool, cOnNull, cOnObjectStart,
                                           cOnObjectEnd, cOnArrayStart, cOnArrayEnd});
  parser.feed(raw.data(), raw.size());
  raw.clear();
  raw.shrink_to_fit();
  if (cards.empty()) return false;

  // Lo que el índice no diga se deduce del id, que es como los deja el servidor.
  categories.clear();
  for (Card& card : cards) {
    if (card.img.empty()) card.img = "cards/img/" + card.id + ".bmp";
    if (card.audioEs.empty()) card.audioEs = "cards/audio/es/" + card.id + ".adp";
    if (card.audioEn.empty()) card.audioEn = "cards/audio/en/" + card.id + ".adp";
    if (card.es.empty()) card.es = card.en;
    if (card.en.empty()) card.en = card.es;
    if (!card.cat.empty() && std::find(categories.begin(), categories.end(), card.cat) == categories.end()) {
      categories.push_back(card.cat);
    }
  }
  LOG_INF(TAG, "%u tarjetas, %u categorías", (unsigned)cards.size(), (unsigned)categories.size());
  return true;
}

void CardsActivity::onEnter() {
  Activity::onEnter();
  randomSeed(millis());
  if (!loadIndex()) {
    state = NO_PACK;
    forceClean = true;
    requestUpdate();
    return;
  }
  category = -1;
  buildDeck();
  state = SHOWING;
  forceClean = true;
  requestUpdate();
}

void CardsActivity::onExit() {
  Activity::onExit();
  stopSpeaking();
}

// ---------------------------------------------------------------------------
// Baraja
// ---------------------------------------------------------------------------

void CardsActivity::buildDeck() {
  deck.clear();
  for (size_t i = 0; i < cards.size(); ++i) {
    if (category < 0 || cards[i].cat == categories[static_cast<size_t>(category)]) {
      deck.push_back(static_cast<int>(i));
    }
  }
  shuffleDeck();
  pos = 0;
}

void CardsActivity::shuffleDeck() {
  for (int i = static_cast<int>(deck.size()) - 1; i > 0; --i) {
    std::swap(deck[static_cast<size_t>(i)], deck[static_cast<size_t>(random(i + 1))]);
  }
}

const CardsActivity::Card* CardsActivity::current() const {
  if (deck.empty() || pos < 0 || pos >= static_cast<int>(deck.size())) return nullptr;
  const int idx = deck[static_cast<size_t>(pos)];
  if (idx < 0 || idx >= static_cast<int>(cards.size())) return nullptr;
  return &cards[static_cast<size_t>(idx)];
}

// Adelante o atrás. Al terminar la baraja se vuelve a barajar: así no se repite
// ninguna tarjeta hasta que salieron todas.
void CardsActivity::showNext(const int delta) {
  if (deck.empty()) return;
  stopSpeaking();  // el I2S es uno solo: lo que estaba sonando se corta
  pos += delta;
  if (pos >= static_cast<int>(deck.size())) {
    shuffleDeck();
    pos = 0;
  } else if (pos < 0) {
    pos = static_cast<int>(deck.size()) - 1;
  }
  forceClean = true;  // un dibujo a pantalla completa parcial deja fantasma
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Voz
// ---------------------------------------------------------------------------

void CardsActivity::startSpeaking() {
  const Card* card = current();
  if (!card) return;
  speech.stop();
  if (!speech.playFile(sdPath(card->audioEs).c_str())) {
    LOG_ERR(TAG, "sin audio en español para %s", card->id.c_str());
    // Sin el clip en español se intenta el inglés, y si tampoco está no pasa nada.
    if (!speech.playFile(sdPath(card->audioEn).c_str())) {
      speakStage = 0;
      return;
    }
    speakStage = 2;
    clipStartedAt = millis();
    return;
  }
  speakStage = 1;
  clipStartedAt = millis();
}

// Primero el español, y cuando termina, el inglés. Sin bloquear: el clip lo
// reproduce la tarea de audio del SDK y acá solo se mira si terminó.
void CardsActivity::pumpSpeaking() {
  if (speakStage == 0) return;
  if (millis() - clipStartedAt < CLIP_SETTLE_MS) return;  // recién arrancó
  if (speech.isPlaying()) return;
  if (speakStage == 1) {
    const Card* card = current();
    speech.stop();
    if (card && speech.playFile(sdPath(card->audioEn).c_str())) {
      speakStage = 2;
      clipStartedAt = millis();
      return;
    }
  }
  speech.stop();
  speakStage = 0;
}

void CardsActivity::stopSpeaking() {
  if (speakStage != 0) {
    speech.stop();
    speakStage = 0;
  }
}

// ---------------------------------------------------------------------------
// Categorías
// ---------------------------------------------------------------------------

void CardsActivity::openCategories() {
  if (categories.empty()) return;
  stopSpeaking();
  pickerOptions.clear();
  pickerOptions.push_back(tr(STR_CARDS_ALL));
  for (const std::string& name : categories) pickerOptions.push_back(name);
  state = CATEGORY;
  picker.show(StrId::STR_CARDS_CATEGORY, pickerOptions, category + 1, [this](const int idx) {
    if (idx >= 0) {
      category = idx - 1;  // 0 = todas
      buildDeck();
    }
    state = SHOWING;
    forceClean = true;
    requestUpdate();
  });
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void CardsActivity::loop() {
  if (state == CATEGORY) {
    if (picker.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (state == CATEGORY && !picker.isActive()) {
        state = SHOWING;
        forceClean = true;
        requestUpdate();
      }
    }
    return;
  }

  if (state == NO_PACK) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // OK lleva directo a bajar el paquete de contenido.
      startActivityForResult(std::make_unique<AssetSyncActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               if (loadIndex()) {
                                 category = -1;
                                 buildDeck();
                                 state = SHOWING;
                               }
                               forceClean = true;
                               requestUpdate();
                             });
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  pumpSpeaking();

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, CATEGORY_HOLD_MS)) {
    openCategories();
    return;
  }
  buttonNavigator.onNext([this] { showNext(1); });
  buttonNavigator.onPrevious([this] { showNext(-1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startSpeaking();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    stopSpeaking();
    finish();
  }
}

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------

void CardsActivity::drawCard(const int top, const int bottom) {
  const int pageWidth = renderer.getScreenWidth();
  const Card* card = current();
  if (!card) {
    renderer.drawCenteredText(UI_12_FONT_ID, (top + bottom) / 2, tr(STR_CARDS_EMPTY), true, EpdFontFamily::BOLD);
    return;
  }

  // El dibujo, centrado y en un marco. Es un BMP de 4 GRISES (el servidor lo
  // arma igual que las fotos), asi que el dibujo en si solo deja la base en
  // blanco y negro: los grises los agrega GrayText::displayPage al final del
  // render. Sin eso el SDK pinta de negro todo lo que no sea blanco puro y la
  // figura sale como silueta.
  const int boxW = std::min(IMAGE_BOX, pageWidth - 40);
  const int boxTop = top + 16;
  bool drawn = false;
  {
    HalFile file;
    const std::string path = sdPath(card->img);
    if (Storage.openFileForRead(TAG, path, file)) {
      Bitmap bitmap(file, false);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int w = std::min(bitmap.getWidth(), boxW);
        const int h = std::min(bitmap.getHeight(), IMAGE_BOX);
        renderer.drawBitmap(bitmap, (pageWidth - w) / 2, boxTop + (IMAGE_BOX - h) / 2, boxW, IMAGE_BOX, 0, 0);
        drawn = true;
      } else {
        LOG_ERR(TAG, "BMP inválido: %s", path.c_str());
      }
      file.close();
    }
  }
  if (!drawn) {
    // Sin dibujo la tarjeta sigue sirviendo: queda el marco y la palabra.
    renderer.drawRoundedRect((pageWidth - boxW) / 2, boxTop, boxW, IMAGE_BOX, 2, 16, true);
    renderer.drawCenteredText(UI_10_FONT_ID, boxTop + IMAGE_BOX / 2 - 10, tr(STR_CARDS_NO_IMAGE));
  }

  int y = boxTop + IMAGE_BOX + 26;
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, y,
                            renderer.truncatedText(NOTOSANS_18_FONT_ID, card->es.c_str(), pageWidth - 40,
                                                   EpdFontFamily::BOLD)
                                .c_str(),
                            true, EpdFontFamily::BOLD);
  y += 46;
  renderer.drawCenteredText(NOTOSANS_16_FONT_ID, y,
                            renderer.truncatedText(NOTOSANS_16_FONT_ID, card->en.c_str(), pageWidth - 40).c_str());
  y += 40;
  if (!card->cat.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, y,
                              renderer.truncatedText(SMALL_FONT_ID, card->cat.c_str(), pageWidth - 40).c_str());
  }

  char pos_[24];
  snprintf(pos_, sizeof(pos_), "%d/%d", pos + 1, static_cast<int>(deck.size()));
  renderer.drawText(SMALL_FONT_ID, 20, bottom - 20, pos_);
  const char* hint = categories.empty() ? tr(STR_CARDS_SAY_HINT) : tr(STR_CARDS_CATEGORY_HINT);
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, hint), bottom - 20, hint);
}

void CardsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_CARDS));

  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - (metrics.buttonHintsHeight + metrics.verticalSpacing);

  const char* confirmLabel = tr(STR_CARDS_SAY);
  const char* upLabel = tr(STR_DIR_UP);
  const char* downLabel = tr(STR_DIR_DOWN);

  switch (state) {
    case SHOWING:
      drawCard(top, bottom);
      break;
    case CATEGORY:
      drawCard(top, bottom);
      if (picker.processRender(renderer, mappedInput)) return;
      break;
    case NO_PACK: {
      const int mid = (top + bottom) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, tr(STR_CARDS_NO_PACK), true, EpdFontFamily::BOLD);
      UITheme::drawCenteredWrappedText(renderer, Rect{20, mid, pageWidth - 40, 120}, UI_10_FONT_ID,
                                       tr(STR_CARDS_NO_PACK_HINT), 3);
      confirmLabel = tr(STR_CARDS_GET_PACK);
      upLabel = "";
      downLabel = "";
      break;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, upLabel, downLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Cada tarjeta es un dibujo a pantalla completa: va con refresco limpio o el
  // panel se queda con la figura anterior encima.
  if (forceClean) {
    partialCount = PARTIALS_BEFORE_CLEAN;
    forceClean = false;
  }
  // UNA sola pasada, no tres.
  //
  // Las tarjetas se dibujaban con los cuatro grises del panel: base en blanco y
  // negro, pasada LSB, pasada MSB y el render por franjas de por medio. Pasar
  // una tarjeta tardaba una eternidad. Ahora el dibujo viene a puro trazo desde
  // el servidor (`renderCard` en assets.ts saca los bordes y nada más), así que
  // no hay grises que mandar y alcanza con el refresco normal.
  drawCard(top, bottom);
  renderer.displayBuffer(GrayText::nextRefreshMode(partialCount));
}
