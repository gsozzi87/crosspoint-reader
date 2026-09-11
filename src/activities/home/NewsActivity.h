#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/TextChunks.h"
#include "voice/SpeechOut.h"

// News: the RSS/Atom feeds loaded from the board page, headlines per feed,
// and the article cleaned to plain text by the server. Headlines are cached on
// the SD (/.crosspoint/rss/feeds.json) and each article read is kept (last
// ones) so they open without WiFi. Back held refreshes the headlines.
//
// El artículo se lee ACÁ (no en el visor de definiciones) porque además de
// mostrarlo hay que poder escucharlo: la pantalla del artículo va por trozos y
// OK arranca la lectura en voz alta. Los clips los sintetiza el servidor
// (GET /api/tts, tope de 4000 caracteres por pedido), así que el texto se parte
// en trozos de una frase entera (util/TextChunks.h) y se piden en orden: uno
// suena mientras se baja el siguiente. Sin WiFi el artículo se lee igual en
// pantalla, solo que no se puede escuchar.
class NewsActivity final : public Activity {
 public:
  explicit NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("News", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING || speaking; }

 private:
  enum State { FEEDS, ITEMS, ARTICLE, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, REFRESH, ARTICLE_FETCH, CLIP };
  State state = FEEDS;
  Pending pending = NONE;

  struct Item {
    int id = 0;
    std::string title;
    std::string when;
  };
  struct Feed {
    int id = 0;
    std::string name;
    std::vector<Item> items;
  };
  std::vector<Feed> feeds;
  int feedIndex = 0;
  int itemIndex = 0;
  int itemsPerPage = 1;
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;

  // --- El artículo abierto y su lectura en voz alta -------------------------
  // Un cuerpo más corto que esto no es una noticia: tiene el tamaño de los
  // mensajes de "no se pudo traer" que antes se guardaban como si fueran la
  // nota. Se usa para reintentar bajarla, nunca para borrarla.
  static constexpr size_t RESCUE_MIN_CHARS = 400;
  // Lo guardado mientras se reintenta, para no quedar con menos que antes.
  std::string rescueTitle;
  std::string rescueText;
  std::string articleTitle;
  std::string articleText;
  std::vector<textchunks::Span> chunks;
  int chunkIndex = 0;
  bool speaking = false;  // la lectura está en marcha (aunque esté en pausa)
  bool paused = false;
  SpeechOut speech;
  // El ADPCM del trozo que suena se guarda para poder reanudarlo: el reproductor
  // no sabe pausar, así que "seguir" vuelve a poner el trozo desde el principio.
  std::string clip;
  std::string nextClip;   // el del trozo siguiente, pedido mientras suena este
  int nextClipIndex = -1;
  unsigned long speakStartedAt = 0;
  std::string notice;  // por qué no se puede leer (sin WiFi, sin voz)

  bool loadCache();
  bool fetchFeeds();
  std::string articlePath(int feed, int item) const;
  bool readArticle(const std::string& path, std::string& title, std::string& text);
  bool fetchArticle(int feed, int item, std::string& title, std::string& text);
  void openArticle();
  void showArticle(const std::string& title, const std::string& text);
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");

  std::string chunkText(int index) const;
  bool fetchClip(int index, std::string& out);
  void requestClip();
  void serveClip();
  void playClip();
  void startSpeaking();
  void stopSpeaking();
  void pumpSpeech();
  void jumpChunk(int delta);
  void renderArticle();
};
