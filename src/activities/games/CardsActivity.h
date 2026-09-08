#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "voice/SpeechOut.h"

// Tarjetas para bebés: un dibujo grande y la palabra en español y en inglés.
// La palanca cambia de tarjeta (adelante y atrás), OK la dice en voz alta
// (primero en español, después en inglés) y Atrás sale. Atrás mantenido elige
// la categoría, si el paquete las trae.
//
// El juego NO trae nada compilado adentro: los dibujos (BMP de 1 bpp) y los
// audios (ADPCM, los mismos que reproduce `SpeechOut`) los genera el servidor y
// los baja `AssetSyncActivity` a la tarjeta. El índice de las tarjetas está en
// `/.crosspoint/cards/index.json`:
//
//   {"version":1,"cards":[{"id":"apple","es":"Manzana","en":"Apple",
//                          "cat":"Comida","img":"cards/img/apple.bmp",
//                          "audioEs":"cards/audio/es/apple.adp",
//                          "audioEn":"cards/audio/en/apple.adp"}, ...]}
//
// Las rutas son relativas a /.crosspoint/ (o absolutas si arrancan con "/"), la
// misma regla que el manifiesto del paquete. Lo que falte se deduce del id
// (cards/img/<id>.bmp, cards/audio/<es|en>/<id>.adp). Sin paquete, la pantalla
// lo dice y ofrece ir a bajarlo, no se rompe.
//
// Ojo con el audio: el I2S es uno solo, así que antes de arrancar un clip
// siempre va `speech.stop()`, y cambiar de tarjeta corta lo que esté sonando.
class CardsActivity final : public Activity {
 public:
  explicit CardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Cards", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return speakStage != 0; }

  // Una tarjeta del índice. Público porque lo llena el lector del índice.
  struct Card {
    std::string id;
    std::string es;
    std::string en;
    std::string cat;
    std::string img;
    std::string audioEs;
    std::string audioEn;
  };

 private:
  enum State : uint8_t { SHOWING, CATEGORY, NO_PACK };

  static constexpr int PARTIALS_BEFORE_CLEAN = 12;  // regla del panel
  static constexpr unsigned long CATEGORY_HOLD_MS = 1000;
  static constexpr unsigned long CLIP_SETTLE_MS = 300;  // el I2S tarda en arrancar: no preguntar antes

  State state = NO_PACK;
  std::vector<Card> cards;
  std::vector<std::string> categories;  // las que trae el índice, sin repetir
  int category = -1;                    // -1 = todas, si no índice en `categories`

  // La baraja: índices barajados de las tarjetas de la categoría elegida. No se
  // repite ninguna hasta que se termina y se vuelve a barajar.
  std::vector<int> deck;
  int pos = 0;

  SpeechOut speech;
  uint8_t speakStage = 0;  // 0 nada, 1 suena el español, 2 suena el inglés
  unsigned long clipStartedAt = 0;

  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  ButtonNavigator buttonNavigator;
  int partialCount = 0;
  bool forceClean = true;

  bool loadIndex();
  void buildDeck();
  void shuffleDeck();
  void showNext(int delta);
  const Card* current() const;
  // Ruta en la tarjeta de un archivo del paquete (relativa a /.crosspoint/).
  static std::string sdPath(const std::string& path);
  void startSpeaking();
  void pumpSpeaking();
  void stopSpeaking();
  void openCategories();
  void drawCard(int top, int bottom);
};
