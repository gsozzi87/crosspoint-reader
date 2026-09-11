#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Reproductor de MP3 de la SD.
//
// SE REHIZO ENTERO EN 1.5.44. La versión anterior partía la pantalla en tres
// "zonas" invisibles (lista, mandos, volumen) y Atrás las iba rotando: nadie
// podía adivinarlo ("ni los botones, encima las indicaciones de como usarlo
// estan mal, no se entiende una pija"). Ahora hay UNA SOLA LISTA vertical, que
// es lo único que este aparato sabe hacer bien con una palanca de arriba/abajo:
//
//   ARRIBA / ABAJO   recorren la lista, siempre
//   OK               hace lo que dice la fila elegida (y la barra de abajo lo repite)
//   ATRÁS            vuelve: de las pistas a las carpetas, de las carpetas al hub
//
// La pantalla tiene pinta de Winamp vertical (barra de título, visor con el
// contador de segmentos y el analizador, barra de posición, botonera de
// transporte, volumen y la lista de pistas abajo), pero por dentro sigue siendo
// UNA sola lista: los seis botones de la botonera son las seis primeras
// posiciones, después el volumen y después las pistas. Por eso la palanca
// recorre la botonera de izquierda a derecha y sigue de largo hacia abajo sin
// que haya "zonas" ni modos escondidos. El volumen sí tiene su modito: OK sobre
// la barra y la palanca sube y baja, OK o Atrás para terminar.
//
// La música NO vive acá: vive en MusicPlayer (src/music/MusicPlayer.h), así
// que salir de esta pantalla no corta la canción y el hub puede mostrar qué
// está sonando.
class MusicActivity final : public Activity {
 public:
  explicit MusicActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Music", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Level { FOLDERS, PLAYLIST };
  // Lo que puede haber en una fila. El orden de ACT_* es el orden en pantalla.
  enum RowKind { ROW_ACTION, ROW_TRACK, ROW_FOLDER };
  // El orden de los seis primeros es el orden de la botonera, de izquierda a
  // derecha; ACT_VOLUME es la fila que va justo debajo.
  enum Action { ACT_PREV, ACT_PLAYPAUSE, ACT_NEXT, ACT_STOP, ACT_SHUFFLE, ACT_REPEAT, ACT_VOLUME };
  static constexpr int TRANSPORT_COUNT = 6;

  struct Row {
    RowKind kind;
    int index;  // pista o carpeta
    Action action;
  };

  Level level = FOLDERS;
  bool volumeMode = false;  // la palanca cambia el volumen en vez de moverse
  int selected = 0;
  int scroll = 0;
  ButtonNavigator buttonNavigator;

  std::vector<std::string> folders;    // rutas completas
  std::vector<std::string> tracks;     // rutas de la carpeta que se está mirando
  std::vector<std::string> trackNames;
  // Duración de cada pista, en segundos. -1 = todavía no se midió; 0 = se midió
  // y el archivo no lo dice. Se llena SOLO con las filas que se ven (una lectura
  // de cabecera por archivo, no las 200 de la carpeta al abrirla).
  std::vector<int> trackSeconds;
  std::string folderName;
  std::string folderPath;
  std::vector<Row> rows;

  int partials = 0;
  int lastShownSecond = -1;
  bool forceClean = false;

  void scanFolders();
  void openFolder(int index);
  void buildRows();
  int visibleRows(int listTop) const;
  void clampScroll(int listTop);
  void moveSelection(int direction);
  void activate();
  void goBack();

  // Qué dice el botón OK para la fila elegida. Nunca "Seleccionar" a secas.
  const char* confirmLabel() const;
  std::string rowLabel(const Row& row) const;
  std::string rowValue(const Row& row) const;  // lo que va a la derecha de la fila
  // Duración de la pista `index`, medida y cacheada la primera vez que la fila
  // se ve. Devuelve 0 cuando el archivo no permite calcularla.
  int trackDuration(int index);

  void drawTitleBar(int x, int y, int w, int h) const;
  void drawDisplay(int x, int y, int w, int h) const;
  void drawAnalyzer(int x, int y, int w, int h) const;
  void drawPosition(int x, int y, int w, int h) const;
  void drawTransport(int x, int y, int w, int h) const;
  void drawVolume(int x, int y, int w, int h) const;
  void drawPlaylist(int x, int y, int w, int h);
  void drawTransportIcon(int action, int cx, int cy) const;
};
