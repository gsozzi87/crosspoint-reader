#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "voice/VoiceRecorder.h"

// Bible: books -> chapters -> paged text, from the server one chapter at a
// time and cached on the SD (/.crosspoint/bible/<lang>/), so anything read
// once is there without WiFi. Back held = ask by voice ("John 3 16",
// "Psalm 23", or words to search); the server resolves the reference or
// returns matching verses to pick from. The last place read is remembered.
class BibleActivity final : public Activity {
 public:
  explicit BibleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bible", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state == RECORDING || state == CONNECTING || state == LOADING; }

 private:
  enum State { BOOKS, CHAPTERS, READING, RECORDING, CONNECTING, LOADING, PICK_RESULT, SEARCHING, FAILED };
  enum Pending { NONE, LOAD_BOOKS, LOAD_CHAPTER, VOICE };
  State state = BOOKS;
  Pending pending = NONE;
  State stateAfterConnect = BOOKS;

  struct BookInfo {
    std::string name;
    int chapters = 0;
  };
  std::vector<BookInfo> books;
  int bookIndex = 0;
  int chapterIndex = 0;  // 0-based
  int wantedVerse = 0;
  int listTop = 0;  // primera fila visible de la lista
  int partialCount = 0;  // parciales desde el último refresco limpio
  ButtonNavigator buttonNavigator;

  VoiceRecorder recorder{8};
  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  struct Hit {
    int book;
    int chapter;
    int verse;
  };
  std::vector<Hit> hits;

  std::string lang;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  std::string dayRef;
  std::string dayText;

  // Biblia entera en la SD: un archivo por libro ("#<capítulo>" y los versículos
  // numerados abajo). Con eso se lee y se busca sin WiFi. Ya NO se baja desde
  // acá: viene en el paquete de contenido (`AssetSyncActivity`), que lo deja en
  // el mismo lugar.
  int booksOnCard = 0;     // libros ya bajados (se recuenta, no se mira la SD en cada dibujo)
  bool offerAssets = false;  // el error se arregla bajando el paquete: se ofrece ir
  int searchIndex = 0;     // libro que se está revisando en una búsqueda offline
  std::string searchQuery;
  std::vector<std::string> searchWords;  // todas tienen que estar en el versículo
  bool offlineSearch = false;

  std::string cacheDir() const;
  std::string bookPath(int book) const;
  bool bookDownloaded(int book) const;
  bool bibleComplete() const;
  // Recuenta los libros que hay en la tarjeta (una pasada por la SD).
  void refreshCardCount();
  bool readChapterFromBook(int book, int chapter, std::string& text) const;
  // Referencia hablada ("Juan 3 16") resuelta con los nombres que ya están en la SD.
  bool parseRefLocal(const std::string& spoken, int& book, int& chapter, int& verse) const;
  // Un paso de la búsqueda offline: revisa un libro y acumula en hits.
  void searchStep();
  void finishSearch();
  bool loadBooksFromCache();
  bool fetchBooks();
  bool chapterCached(int book, int chapter) const;
  bool fetchChapter(int book, int chapter, std::string& text);
  bool readChapter(int book, int chapter, std::string& text);
  void openChapter(int book, int chapter, int verse);
  void showChapter(const std::string& text);
  void ensureConnected(State next);
  void onWifiSelectionComplete(bool connected);
  void startVoice();
  void performVoice();
  void fail(StrId why, std::string detail = "");
  void saveLastRef();
};
