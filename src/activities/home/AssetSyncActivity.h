#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/home/HubSyncActivity.h"  // FriendlyWifi

// Paquete de contenido: todo lo pesado (los dibujos y los audios de las
// tarjetas de bebé, los sonidos, la Biblia entera, los íconos) vive en el
// servidor y se baja de una sola vez, después de actualizar el firmware o
// cuando el usuario lo pide desde Ajustes → Descargar contenido.
//
//   GET /api/assets/manifest?lang=&v=  →  {version, items:[{id,kind,path,bytes,sha}]}
//   GET /api/assets/file?id=           →  el archivo tal cual
//
// El manifiesto de lo que quedó en la tarjeta se guarda en
// `/.crosspoint/assets.json` ({"version","lang","items":{"<id>":"<sha>"}}), así
// la próxima vez solo se baja lo que falta o lo que cambió. Un archivo por
// pasada del loop, para que la pantalla siga viva; Atrás corta la descarga y
// conserva lo bajado (la próxima vez sigue desde ahí).
//
// Ojo con `ServerClient`: no sabe de `Range` ni de bajar a archivo en streaming,
// así que cada archivo se baja entero en memoria y se escribe de una. Los
// archivos del paquete tienen que ser chicos (un dibujo de 320x320 a 1 bpp son
// 13 KB, un clip de voz de 2 s en ADPCM son 16 KB); lo que pase de MAX_FILE_KB
// se saltea y se avisa.
class AssetSyncActivity final : public Activity {
 public:
  // `wifiReady` = la pantalla que nos abrió ya tiene el WiFi arriba y se
  // encarga de bajarlo al salir (la actualización de firmware). Sin eso,
  // levantamos el WiFi nosotros y salimos con silentRestart().
  explicit AssetSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool wifiReady = false)
      : Activity("AssetSync", renderer, mappedInput), wifiReady(wifiReady) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == MANIFEST || state == DOWNLOADING; }
  bool skipLoopDelay() override { return state == DOWNLOADING; }

  // Un archivo del manifiesto del servidor.
  struct Item {
    std::string id;
    std::string kind;  // bible | cards | sounds | icons
    std::string path;  // ruta que manda el servidor (relativa a /.crosspoint/ o absoluta)
    std::string sha;
    uint32_t bytes = 0;
    bool needed = false;   // falta o cambió: hay que bajarlo
    bool present = false;  // está en la tarjeta con este sha (entra en el manifiesto local)
  };

  // Después de instalar firmware nuevo: queda anotado que falta bajar el
  // contenido, para ofrecerlo cuando el aparato arranque con la versión nueva.
  static void markPending();
  static bool isPending();
  // ¿Hay algo del paquete en la tarjeta? Lo usan la Biblia y las tarjetas para
  // decir "todavía no bajaste el contenido" en vez de romperse.
  static bool hasPackage();

 private:
  enum State { CONNECTING, MANIFEST, DOWNLOADING, DONE, FAILED };

  State state = CONNECTING;
  bool wifiReady = false;   // el WiFi lo levantó quien nos abrió
  bool ownsWifi = false;    // lo levantamos nosotros: hay que reiniciar al salir
  FriendlyWifi wifi;
  bool wifiPicker = false;  // la pantalla de selección de red tiene el foco

  std::vector<Item> items;
  std::string version;   // versión del paquete que manda el servidor
  std::string lang;
  size_t index = 0;      // ítem que se está mirando
  int tries = 0;         // intentos del ítem actual
  int needCount = 0;     // cuántos hay que bajar en total
  int doneCount = 0;     // cuántos se bajaron ya
  int failCount = 0;     // los que no se pudieron bajar
  uint32_t needBytes = 0;
  uint32_t doneBytes = 0;
  bool stopped = false;   // el usuario cortó con Atrás
  bool upToDate = false;  // no había nada que bajar
  int savedSince = 0;     // archivos bajados desde la última vez que se guardó el manifiesto local

  std::string localRaw;  // el manifiesto local, crudo, para buscar el sha de un id
  StrId failureId = StrId::STR_ASSETS_FAILED;
  std::string failureDetail;

  unsigned long lastPaintAt = 0;
  int lastPercent = -1;
  int partialCount = 0;
  bool forceClean = true;

  void beginConnect();
  void pumpConnect();
  void onWifiReady(bool connected);
  void fetchManifest();
  void downloadStep();
  bool downloadItem(const Item& item);
  void finishDownload();
  void saveLocalManifest();
  void fail(StrId why, std::string detail = "");
  int percent() const;
  // Repinta solo cuando cambió algo que se ve y no más seguido que cada tanto:
  // el panel no aguanta un refresco por archivo.
  void paintProgress(bool force = false);
};
