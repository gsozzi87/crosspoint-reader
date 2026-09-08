#pragma once

#include <AudioManager.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "music/Mp3Source.h"
#include "util/ButtonNavigator.h"

// Reproductor de MP3 de la SD, pensado para los botones que tiene el aparato:
// una palanca (arriba XOR abajo), OK y Atrás. No hay rueda de volumen ni forma
// de "apuntar" a un botón de la pantalla, así que la pantalla se divide en tres
// zonas y la palanca trabaja siempre dentro de la zona que tiene el foco:
//
//   Lista    — arriba/abajo elige carpeta o pista, OK abre / reproduce / pausa
//   Control  — arriba/abajo elige el mando (Play, Anterior, Siguiente, Detener,
//              Aleatorio, Repetir) y OK lo activa
//   Volumen  — arriba sube y abajo baja el volumen, OK pausa o reanuda
//
// Atrás corto pasa a la zona siguiente y Atrás mantenido sale (de la carpeta
// primero, del reproductor después). La barra de abajo dice siempre qué hace
// cada botón en la zona que está enfocada, y el volumen es el único del aparato
// (HubStore::musicVolume): el mismo de la voz, los avisos y la música.
class MusicActivity final : public Activity {
 public:
  explicit MusicActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Music", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return playing && !paused; }

 private:
  enum Level { FOLDERS, PLAYLIST };
  // Zona con el foco. El orden es el del ciclo de Atrás corto.
  enum Zone { ZONE_LIST = 0, ZONE_TRANSPORT, ZONE_VOLUME, ZONE_COUNT };
  // Mandos de la zona Control, en el orden en que los recorre la palanca.
  enum Control { CTRL_PLAY = 0, CTRL_PREV, CTRL_NEXT, CTRL_STOP, CTRL_SHUFFLE, CTRL_REPEAT, CTRL_COUNT };

  Level level = FOLDERS;
  Zone zone = ZONE_LIST;
  int controlIndex = CTRL_PLAY;

  std::vector<std::string> folders;  // full paths
  std::vector<std::string> tracks;   // full paths of the folder being browsed
  std::vector<std::string> trackNames;
  std::string folderName;
  int folderIndex = 0;
  int trackIndex = 0;  // selected in the list
  ButtonNavigator buttonNavigator;

  // Lo que suena vive aparte de lo que se está mirando: Atrás vuelve a las
  // carpetas y la música sigue, así que la vista no puede depender del vector
  // de la carpeta abierta (una carpeta más corta daba lectura fuera de rango).
  std::vector<std::string> playTracks;
  std::vector<std::string> playTrackNames;
  std::string playFolderName;
  int playFolderIndex = -1;  // carpeta que suena, -1 = ninguna
  int playingIndex = -1;     // pista cargada, índice en playTracks

  AudioManager audio;
  Mp3Source source;
  bool playing = false;
  bool paused = false;
  bool shuffle = false;
  bool repeat = false;
  int volume = 70;
  int lastShownSecond = -1;
  int partials = 0;

  // Cierto cuando la fila seleccionada es la pista que está sonando (misma
  // carpeta y mismo índice): sin lo primero, OK sobre otra carpeta pausaba.
  bool isSelectedPlaying() const {
    return playing && folderIndex == playFolderIndex && trackIndex == playingIndex;
  }
  int listCount() const;

  void scanFolders();
  void openFolder(int index);
  bool playSelected(int index);  // adopta la carpeta que se está mirando y arranca
  bool play(int index);          // índice en playTracks (la carpeta que suena)
  void stop();
  void togglePause();
  void playPause();  // OK sobre el mando Play y sobre la zona de volumen
  void next(bool fromEnd);
  void previous();
  void setVolume(int value);

  // Entradas, ya repartidas por zona.
  void step(int direction);
  void activate();
  void leaveOrExit();

  // Etiquetas de la barra de botones: siempre dicen lo que hace cada botón AHORA.
  const char* confirmLabel() const;
  const char* zoneLabel(Zone z) const;
  const char* controlLabel(int control) const;

  int drawNowPlaying(int x, int y, int w) const;      // devuelve el alto usado
  int drawControls(int x, int y, int w) const;        // idem
  int drawVolume(int x, int y, int w) const;          // idem
  void drawList(int x, int y, int w, int h) const;
};
