#pragma once

#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Fondo de pantalla: elegir QUÉ foto queda pintada cuando el aparato se
// suspende. No es un visor de fotos. La lista baja del servidor lo que se subió
// desde /board (`GET /api/photos`), guarda los BMP en /Photos de la SD y OK
// abre la vista previa; ahí OK deja esa foto como fondo (`HubStore::wallpaperPath`,
// la pinta `enterDeepSleep()` en main.cpp). La primera fila, "Ninguno", vuelve a
// la pantalla de sueño de siempre. Atrás mantenido actualiza la lista.
class PhotosActivity final : public Activity {
 public:
  explicit PhotosActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wallpaper", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

  // Pinta una foto a pantalla completa con el pipeline de grises del SDK
  // (base BW + plano LSB + plano MSB + displayGrayBuffer). La usan la vista
  // previa de acá y el fondo de suspensión de main.cpp. Devuelve false sin
  // tocar la pantalla si el archivo no está o el BMP no se puede leer.
  // `baseOverlay` se llama con la foto ya dibujada y antes del refresco, para
  // agregarle algo encima (la barra de botones de la previa).
  static bool drawFullScreenPhoto(GfxRenderer& renderer, const std::string& path,
                                  const std::function<void()>& baseOverlay = nullptr);

 private:
  enum State { LIST, PREVIEW, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, REFRESH, DOWNLOAD };
  State state = LIST;
  Pending pending = NONE;

  struct Photo {
    std::string id;    // server id, empty for a local file
    std::string name;
    std::string path;  // on the SD when downloaded
    bool local = false;
  };
  std::vector<Photo> photos;
  // Fila seleccionada: 0 = "Ninguno", 1..photos.size() = cada foto.
  int index = 0;
  int itemsPerPage = 1;
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  std::string lastError;  // "Server 401", "Transport 0": el motivo real del último pedido

  int rowCount() const { return 1 + static_cast<int>(photos.size()); }
  int photoAt(int row) const { return row - 1; }  // < 0 = la fila "Ninguno"
  bool isWallpaper(const Photo& photo) const;

  void scanLocal();
  bool fetchList();
  bool download(Photo& photo);
  void confirmCurrent();
  void setWallpaper(const Photo* photo);
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");
  void drawPreview();
};
