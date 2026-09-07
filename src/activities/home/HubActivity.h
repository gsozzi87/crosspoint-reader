#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// ws397 home: a grid of big tiles (Read, Ask, Reminders, Bible, Music,
// Settings) under a status line (clock, battery), plus a "continue reading"
// widget. The classic CrossPoint home (file browser, recents, transfer) sits
// behind the Read tile; the reader is unchanged. UP/DOWN walk the tiles, OK
// opens, Back resumes the last book. Ready for the trackball: the tile order
// is row-major so a 2D cursor maps onto the same index.
class HubActivity final : public Activity {
 public:
  explicit HubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool cleanInitialRefresh = false)
      : Activity("Hub", renderer, mappedInput), cleanInitialRefresh(cleanInitialRefresh) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  // Clima ocupa el lugar que tenía Juegos (que todavía dice "Próximamente"): en
  // esta placa OK no tiene pulsación larga (OK mantenido apaga), así que el
  // "OK largo: clima" del hub nunca se podía usar.
  enum Tile { TILE_READ = 0, TILE_TALK, TILE_TRANSLATOR, TILE_REMINDERS, TILE_TIMER, TILE_NOTES, TILE_BIBLE, TILE_MUSIC, TILE_NEWS, TILE_PHOTOS, TILE_WEATHER, TILE_SETTINGS, TILE_COUNT };
  static constexpr int COLUMNS = 3;

  ButtonNavigator buttonNavigator;
  const bool cleanInitialRefresh;
  int selected = TILE_READ;
  bool firstRenderDone = false;
  bool comingSoon = false;  // a not-yet-built tile was opened: show the notice
  bool autoSyncPending = false;  // cache stale at entry: run HubSyncActivity after the first paint
  unsigned long lastClockMinuteTick = 0;
  char lastClock[9] = {0};
  char lastTimeChip[40] = {0};  // lo último dibujado del temporizador/cronómetro
  // Texto del temporizador o el cronómetro para la barra de estado ("" si no hay nada).
  void formatTimeChip(char* out, size_t size) const;

  // Most recent book still on the card (the Back shortcut + the widget).
  std::string lastBookPath;
  std::string lastBookTitle;
  std::string lastBookAuthor;

  void loadLastBook();
  bool shouldAutoSync() const;
  void startSync();
  void activate(int tile);
  void drawStatusLine(int y, int height) const;
  void drawTile(int index, int x, int y, int w, int h) const;
  void drawContinueWidget(int x, int y, int w, int h) const;
  void drawInfoWidgets(int x, int y, int w, int h) const;
};
