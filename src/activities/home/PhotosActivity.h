#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Photo album: 4-gray BMPs uploaded from the board page (the phone's browser
// does the scaling and dithering, so the device only downloads and draws).
// Each photo is kept on the SD under /Photos, so the album works offline; the
// list also shows BMPs already on the card. OK opens one full screen, UP/DOWN
// move between photos there too, Back held refreshes the list from the server.
class PhotosActivity final : public Activity {
 public:
  explicit PhotosActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Photos", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

 private:
  enum State { LIST, VIEW, CONNECTING, LOADING, FAILED };
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
  int index = 0;
  int itemsPerPage = 1;
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;
  std::string lastError;  // "Server 401", "Transport 0": el motivo real del último pedido

  void scanLocal();
  bool fetchList();
  bool download(Photo& photo);
  void openCurrent();
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");
  void drawPhoto();
};
