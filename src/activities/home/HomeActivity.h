#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // REV-088: en la ws397 la fila "Transferir archivos" NO va.
  //
  // La home clásica cuelga del mosaico **Leer**, así que desde ahí la
  // transferencia se siente como una función del lector — y desde esta versión
  // la puerta canónica de administrar archivos es **Ajustes → Archivos → Modo
  // memoria USB**. Dos puertas visibles a lo mismo, una de ellas adentro de
  // leer, es peor que ninguna.
  //
  // OJO, y esto hay que saberlo: la fila no abría SÓLO el modo USB. Abría
  // `NetworkModeSelectionActivity`, que además del USB ofrece la web por WiFi,
  // Calibre y el punto de acceso. En la ws397 esos tres caminos quedan sin
  // puerta — es lo decidido (la subida de archivos por la web está descartada
  // desde el modo memoria USB), pero no es "esconder un duplicado".
  //
  // La condición vive ACÁ y en un solo lugar: las tres funciones de abajo, el
  // conteo y el render la consultan, así que ninguna se puede separar de las
  // otras y correr los índices. Es la misma trampa de las rutas protegidas
  // escritas dos veces en 1.5.91.
  static bool showsFileTransfer() { return !BoardConfig::isWS397(); }

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    // Sin la fila, un FILE_TRANSFER que llegue de otro lado cae en 0, que es la
    // primera fila: no hay índice para algo que no está en la pantalla.
    if (item == HomeMenuItem::FILE_TRANSFER) return showsFileTransfer() ? i : 0;
    if (showsFileTransfer()) ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (showsFileTransfer() && idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
