#pragma once

#include <AudioManager.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "music/Mp3Source.h"
#include "util/ButtonNavigator.h"

// MP3 player from the SD with the classic Winamp look: main window (title
// marquee, big time counter, kbps/kHz, position bar, transport row, shuffle
// and repeat flags) over a numbered playlist. Folders under /Music are the
// playlists. UP/DOWN move in the playlist, OK plays the selected track (or
// pauses the current one), Back returns to the folders, Back held opens the
// menu (stop, next, previous, shuffle, repeat, volume).
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
  Level level = FOLDERS;
  std::vector<std::string> folders;  // full paths
  std::vector<std::string> tracks;   // full paths of the open folder
  std::vector<std::string> trackNames;
  std::string folderName;
  int folderIndex = 0;
  int trackIndex = 0;    // selected in the list
  int playingIndex = -1;  // currently loaded track
  ButtonNavigator buttonNavigator;

  AudioManager audio;
  Mp3Source source;
  bool playing = false;
  bool paused = false;
  bool shuffle = false;
  bool repeat = false;
  int volume = 70;
  unsigned long lastTick = 0;
  int lastShownSecond = -1;
  int partials = 0;

  OptionPopup menu;
  std::vector<std::string> menuOptions;
  bool menuOpen = false;

  void scanFolders();
  void openFolder(int index);
  bool play(int index);
  void stop();
  void togglePause();
  void next(bool fromEnd);
  void previous();
  void openMenu();
  void onMenuPick(int index);
  void drawMainWindow(int x, int y, int w, int h) const;
  void drawPlaylist(int x, int y, int w, int h) const;
};
