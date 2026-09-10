#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// ws397 home: la primera pantalla y la que más se mira. De arriba a abajo:
// barra de estado (hora grande, fecha, temporizador, WiFi, batería), sumario
// (clima, próximo recordatorio, lo que suena o el libro abierto, agenda o
// frase) y la grilla de mosaicos. Cada franja se separa de la siguiente con una
// regla de 1 px: cuesta nada de tinta y no fantasmea, que es lo que sí hacían
// los marcos y los bloques rellenos.
//
// La palanca ARRIBA/ABAJO recorre los mosaicos en orden de lectura, OK abre y
// Atrás no hace nada (el hub es el fondo de todo; Atrás mantenido sincroniza).
class HubActivity final : public Activity {
 public:
  explicit HubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool cleanInitialRefresh = false)
      : Activity("Hub", renderer, mappedInput), cleanInitialRefresh(cleanInitialRefresh) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  // Catorce mosaicos en tres columnas no dan filas parejas, así que la primera
  // fila la comparten "Mi día" — ancho, ocupa dos columnas, con su subtítulo —
  // y "Conversor". Debajo quedan las cuatro filas de tres de siempre. El orden
  // del enum ES el orden de lectura: la palanca recorre índices, no coordenadas.
  //
  // Ningún mosaico dice ya "Próximamente": Juegos abre los juegos desde 1.5.47 y
  // el Conversor abre la app de unidades.
  enum Tile {
    TILE_DAY = 0,  // fila 0, columnas 0 y 1
    TILE_UNITS,    // fila 0, columna 2
    TILE_READ,
    TILE_TALK,
    TILE_TRANSLATOR,
    TILE_REMINDERS,
    TILE_TIMER,
    TILE_NOTES,
    TILE_BIBLE,
    TILE_MUSIC,
    TILE_NEWS,
    TILE_GAMES,
    TILE_WEATHER,
    TILE_SETTINGS,
    TILE_COUNT
  };
  static constexpr int COLUMNS = 3;
  static constexpr int GRID_ROWS = 5;  // la de "Mi día" + Conversor, y cuatro de mosaicos

  ButtonNavigator buttonNavigator;
  const bool cleanInitialRefresh;
  int selected = TILE_READ;  // se arranca en Leer, no en el ancho
  bool firstRenderDone = false;
  bool autoSyncPending = false;  // cache stale at entry: run HubSyncActivity after the first paint
  unsigned long lastTick = 0;
  // El hub es la pantalla que más tiempo queda a la vista y se repinta sola
  // cuando cambia el minuto: sin este contador acumula parciales y fantasmea.
  // Lo último que se dibujó de lo que cambia solo (hora, temporizador, música):
  // el tick repinta únicamente cuando esto cambia.
  char lastSignature[96] = {0};
  void screenSignature(char* out, size_t size) const;
  // Texto del temporizador o el cronómetro para la barra de estado ("" si no hay nada).
  void formatTimeChip(char* out, size_t size) const;
  // Fechas para la barra de estado, de más larga a más corta ("Miércoles 9
  // Septiembre", "Mi 9 Septiembre", "Mi 9/9"). Se dibuja la primera que entra
  // en el hueco que quede; vacío si el aparato no está en hora.
  std::vector<std::string> dateCandidates() const;
  // "Interior 23° · 45 %" del SHTC3, o "" si el sensor todavía no midió.
  std::string indoorLine() const;

  // Most recent book still on the card (el renglón del sumario y el mosaico Leer).
  std::string lastBookPath;
  std::string lastBookTitle;
  std::string lastBookAuthor;

  void loadLastBook();
  bool shouldAutoSync() const;
  void startSync();
  void activate(int tile);
  void drawStatusLine(int y, int height) const;
  void drawSummary(int x, int y, int w, int h) const;
  void drawWeatherRow(int x, int y, int w, int h) const;
  void drawReminderRow(int x, int y, int w, int h) const;
  void drawMediaRow(int x, int y, int w, int h) const;
  void drawAgendaRow(int x, int y, int w, int h) const;
  void drawGrid(int x, int y, int w, int h) const;
  void drawTile(int index, int x, int y, int w, int h) const;
  void drawWideTile(int index, int x, int y, int w, int h) const;
};
