#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "voice/VoiceRecorder.h"

// Bible: books -> chapters -> paged text, from the server one chapter at a
// time and cached on the SD (/.crosspoint/bible/<lang>/), so anything read
// once is there without WiFi. The last place read is remembered.
//
// Atrás mantenido es el botón de voz:
//   - en la lista de libros graba y busca ("Juan 3 16", "Salmo 23", o palabras
//     sueltas); con la Biblia entera en la tarjeta eso se resuelve acá y el
//     servidor solo hace falta para pasar la voz a texto;
//   - en la lista de capítulos abre un menú con "Buscar por voz" y "Preguntar
//     sobre este capítulo" (POST /api/bible/ask con el capítulo entero: "no
//     entendí del versículo 8 al 12", "qué significa esa palabra"). Preguntar
//     es lo único que necesita conexión sí o sí.
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
  enum State { BOOKS, CHAPTERS, READING, MENU, RECORDING, CONNECTING, LOADING, PICK_RESULT, SEARCHING, FAILED };
  enum Pending { NONE, LOAD_BOOKS, LOAD_CHAPTER, VOICE, ASK };
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

  // 20 s: una cita entra en dos, pero una pregunta sobre el capítulo ("no
  // entendí del versículo 8 al 12, ¿qué quiso decir?") no.
  VoiceRecorder recorder{20};
  bool asking = false;         // la toma en curso es una pregunta, no una búsqueda
  bool askAfterViewer = false; // Atrás mantenido dentro del capítulo: al cerrar el visor, menú de voz
  int shownSecond = -1;        // último segundo pintado del contador de grabación
  bool forceClean = false;     // pedir un refresco limpio al entrar/salir de la grabación
  bool suppressAssetOffer = false;  // este error no se arregla bajando el paquete
  OptionPopup picker;
  std::vector<std::string> pickerOptions;
  std::vector<std::string> menuOptions;
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
  void openVoiceMenu();
  void startVoice(bool ask);
  void performVoice();
  // Transcribe, manda el capítulo entero a POST /api/bible/ask y muestra la
  // respuesta paginada con la pregunta como título.
  void performAsk();
  void fail(StrId why, std::string detail = "");
  void saveLastRef();
};
