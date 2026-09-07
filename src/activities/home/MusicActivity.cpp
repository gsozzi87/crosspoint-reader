#include "MusicActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "HubStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "MUSIC";
constexpr const char* MUSIC_ROOT = "/Music";
constexpr int MAX_TRACKS = 200;
constexpr unsigned long MENU_HOLD_MS = 1200;
constexpr int COUNTER_TICK_S = 5;   // partial refresh of the counter
constexpr int PARTIALS_BEFORE_CLEAN = 24;

bool endsWithMp3(const char* name) {
  const size_t len = strlen(name);
  if (len < 4) return false;
  const char* ext = name + len - 4;
  return (ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'p' || ext[2] == 'P') && ext[3] == '3');
}

// 7-segment digit (a b c d e f g), half the size of the timer's.
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
void drawDigit(const GfxRenderer& r, int digit, int x, int y, int w, int h, int t, bool ink) {
  const uint8_t s = SEGMENTS[digit % 10];
  const int half = h / 2;
  if (s & 0x01) r.fillRect(x + t, y, w - 2 * t, t, ink);
  if (s & 0x02) r.fillRect(x + w - t, y + t, t, half - t, ink);
  if (s & 0x04) r.fillRect(x + w - t, y + half, t, half - t, ink);
  if (s & 0x08) r.fillRect(x + t, y + h - t, w - 2 * t, t, ink);
  if (s & 0x10) r.fillRect(x, y + half, t, half - t, ink);
  if (s & 0x20) r.fillRect(x, y + t, t, half - t, ink);
  if (s & 0x40) r.fillRect(x + t, y + half - t / 2, w - 2 * t, t, ink);
}

// Winamp-style bevelled box: light top/left, dark bottom/right (in 1-bit: a
// double frame with the inner corner open).
void bevel(const GfxRenderer& r, int x, int y, int w, int h) {
  r.drawRect(x, y, w, h, true);
  r.drawLine(x + 2, y + h - 3, x + w - 3, y + h - 3, true);
  r.drawLine(x + w - 3, y + 2, x + w - 3, y + h - 3, true);
}

void triangle(const GfxRenderer& r, int x, int y, int size, bool right, bool ink) {
  for (int i = 0; i < size; ++i) {
    const int len = right ? size - i : i + 1;
    const int sx = right ? x : x + size - len;
    r.fillRect(sx, y + i, len, 1, ink);
    r.fillRect(sx, y + 2 * size - 1 - i, len, 1, ink);
  }
}
}  // namespace

void MusicActivity::onEnter() {
  Activity::onEnter();
  volume = HUB_STORE.musicVolume > 0 ? HUB_STORE.musicVolume : 70;
  scanFolders();
  level = FOLDERS;
  requestUpdate();
}

void MusicActivity::onExit() {
  Activity::onExit();
  stop();
  audio.end();
}

void MusicActivity::scanFolders() {
  folders.clear();
  auto root = Storage.open(MUSIC_ROOT);
  if (!root || !root.isDirectory()) return;
  root.rewindDirectory();
  char name[128];
  bool rootHasMp3 = false;
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (name[0] == '.') continue;
    if (entry.isDirectory()) folders.push_back(std::string(MUSIC_ROOT) + "/" + name);
    else if (endsWithMp3(name)) rootHasMp3 = true;
  }
  std::sort(folders.begin(), folders.end());
  if (rootHasMp3) folders.insert(folders.begin(), MUSIC_ROOT);
}

void MusicActivity::openFolder(const int index) {
  if (index < 0 || index >= static_cast<int>(folders.size())) return;
  tracks.clear();
  trackNames.clear();
  const std::string& path = folders[index];
  folderName = path == MUSIC_ROOT ? std::string("/") : path.substr(path.find_last_of('/') + 1);
  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) return;
  dir.rewindDirectory();
  char name[128];
  for (auto entry = dir.openNextFile(); entry && tracks.size() < MAX_TRACKS; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() || name[0] == '.' || !endsWithMp3(name)) continue;
    tracks.push_back(path + "/" + name);
  }
  std::sort(tracks.begin(), tracks.end());
  for (const std::string& t : tracks) {
    std::string n = t.substr(t.find_last_of('/') + 1);
    n.resize(n.size() - 4);
    trackNames.push_back(n);
  }
  folderIndex = index;
  trackIndex = 0;
  level = PLAYLIST;
  requestUpdate();
}

bool MusicActivity::play(const int index) {
  if (index < 0 || index >= static_cast<int>(tracks.size())) return false;
  audio.stop();
  if (!source.open(tracks[index])) {
    LOG_ERR(TAG, "cannot open %s", tracks[index].c_str());
    return false;
  }
  if (!audio.begin()) return false;
  audio.setVolume(volume);
  if (!audio.play(source.wavSource(), false)) {
    source.close();
    return false;
  }
  playingIndex = index;
  trackIndex = index;
  playing = true;
  paused = false;
  lastShownSecond = -1;
  partials = PARTIALS_BEFORE_CLEAN;  // clean refresh on a new track
  requestUpdate();
  return true;
}

void MusicActivity::stop() {
  if (playing) audio.stop();
  source.close();
  playing = false;
  paused = false;
  playingIndex = -1;
}

// The SDK has no pause: stopping and replaying restarts the track, so pause
// mutes and keeps the stream (the decoder keeps feeding silence-free PCM).
void MusicActivity::togglePause() {
  if (!playing) return;
  paused = !paused;
  audio.setVolume(paused ? 0 : volume);
  requestUpdate();
}

void MusicActivity::next(const bool fromEnd) {
  if (tracks.empty()) return;
  int idx;
  if (shuffle && tracks.size() > 1) {
    do {
      idx = static_cast<int>(esp_random() % tracks.size());
    } while (idx == playingIndex);
  } else {
    idx = playingIndex + 1;
    if (idx >= static_cast<int>(tracks.size())) {
      if (!repeat && fromEnd) {
        stop();
        requestUpdate();
        return;
      }
      idx = 0;
    }
  }
  play(idx);
}

void MusicActivity::previous() {
  if (tracks.empty()) return;
  int idx = playingIndex - 1;
  if (idx < 0) idx = static_cast<int>(tracks.size()) - 1;
  play(idx);
}

void MusicActivity::openMenu() {
  menuOptions = {tr(STR_MUSIC_STOP),    tr(STR_MUSIC_NEXT),   tr(STR_MUSIC_PREV),
                 shuffle ? tr(STR_MUSIC_SHUFFLE_OFF) : tr(STR_MUSIC_SHUFFLE_ON),
                 repeat ? tr(STR_MUSIC_REPEAT_OFF) : tr(STR_MUSIC_REPEAT_ON),
                 tr(STR_MUSIC_VOL_UP), tr(STR_MUSIC_VOL_DOWN)};
  menuOpen = true;
  menu.show(StrId::STR_HUB_MUSIC, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
  requestUpdate();
}

void MusicActivity::onMenuPick(const int index) {
  menuOpen = false;
  switch (index) {
    case 0: stop(); break;
    case 1: next(false); break;
    case 2: previous(); break;
    case 3: shuffle = !shuffle; break;
    case 4: repeat = !repeat; break;
    case 5: volume = std::min(100, volume + 10); audio.setVolume(paused ? 0 : volume); HUB_STORE.musicVolume = volume; HUB_STORE.saveToFile(); break;
    case 6: volume = std::max(0, volume - 10); audio.setVolume(paused ? 0 : volume); HUB_STORE.musicVolume = volume; HUB_STORE.saveToFile(); break;
    default: break;
  }
  requestUpdate();
}

void MusicActivity::loop() {
  if (menuOpen) {
    if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!menu.isActive() && menuOpen) {
        menuOpen = false;
        requestUpdate();
      }
    }
    return;
  }
  // Track ended: the audio task exits when the source runs dry.
  if (playing && !paused && !audio.isPlaying()) {
    next(true);
    return;
  }
  // Counter: one partial refresh every few seconds while playing.
  if (playing && !paused) {
    const int sec = source.positionSeconds();
    if (sec / COUNTER_TICK_S != lastShownSecond / COUNTER_TICK_S) requestUpdate();
  }

  const int count = level == FOLDERS ? static_cast<int>(folders.size()) : static_cast<int>(tracks.size());
  int& index = level == FOLDERS ? folderIndex : trackIndex;
  if (level == PLAYLIST && mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
    openMenu();
    return;
  }
  buttonNavigator.onNext([&] {
    if (count > 0) index = ButtonNavigator::nextIndex(index, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([&] {
    if (count > 0) index = ButtonNavigator::previousIndex(index, count);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (level == FOLDERS) {
      openFolder(folderIndex);
    } else if (playing && trackIndex == playingIndex) {
      togglePause();
    } else {
      play(trackIndex);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (level == PLAYLIST) {
      level = FOLDERS;  // music keeps playing; the folder list shows what is on
      requestUpdate();
    } else {
      activityManager.goHome();
    }
  }
}

void MusicActivity::drawMainWindow(const int x, const int y, const int w, const int h) const {
  bevel(renderer, x, y, w, h);
  // Title bar
  renderer.fillRect(x + 3, y + 3, w - 6, 18, true);
  renderer.drawText(SMALL_FONT_ID, x + 10, y + 5, "WS397AMP", false);
  // Time counter: mm:ss with 7-segment digits
  const int sec = playing ? source.positionSeconds() : 0;
  const int mm = sec / 60, ss = sec % 60;
  const int dw = 26, dh = 46, t = 6, gap = 6;
  int cx = x + 16, cy = y + 34;
  drawDigit(renderer, (mm / 10) % 10, cx, cy, dw, dh, t, true); cx += dw + gap;
  drawDigit(renderer, mm % 10, cx, cy, dw, dh, t, true); cx += dw + gap;
  renderer.fillRect(cx + 2, cy + 13, t, t, true);
  renderer.fillRect(cx + 2, cy + 29, t, t, true);
  cx += t + 8;
  drawDigit(renderer, ss / 10, cx, cy, dw, dh, t, true); cx += dw + gap;
  drawDigit(renderer, ss % 10, cx, cy, dw, dh, t, true);
  // Marquee: "artist - title", then kbps / kHz / mode
  const int textX = cx + dw + 16;
  const int textW = x + w - 12 - textX;
  std::string line = playing ? (source.artist().empty() ? source.title() : source.artist() + " - " + source.title())
                             : std::string(tr(STR_MUSIC_STOPPED));
  if (playing && playingIndex >= 0) line = std::to_string(playingIndex + 1) + ". " + line;
  renderer.drawRect(textX - 4, cy - 2, textW + 8, 26, true);
  renderer.drawText(UI_10_FONT_ID, textX, cy + 2, renderer.truncatedText(UI_10_FONT_ID, line.c_str(), textW, EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
  char info[64];
  if (playing) {
    snprintf(info, sizeof(info), "%d kbps  %d kHz  %s", source.bitrateKbps(), source.sampleRate() / 1000,
             source.channels() == 2 ? "stereo" : "mono");
  } else {
    snprintf(info, sizeof(info), "%s", folderName.c_str());
  }
  renderer.drawText(SMALL_FONT_ID, textX, cy + 30, renderer.truncatedText(SMALL_FONT_ID, info, textW).c_str());
  // Status flags
  const int flagsY = cy + 30;
  const int flagX = x + w - 12 - 70;
  renderer.drawText(SMALL_FONT_ID, flagX, flagsY, "SHUF", shuffle);
  if (shuffle) renderer.fillRect(flagX - 3, flagsY - 1, 34, 14, true), renderer.drawText(SMALL_FONT_ID, flagX, flagsY, "SHUF", false);
  renderer.drawText(SMALL_FONT_ID, flagX + 40, flagsY, "REP", true);
  if (repeat) renderer.fillRect(flagX + 37, flagsY - 1, 28, 14, true), renderer.drawText(SMALL_FONT_ID, flagX + 40, flagsY, "REP", false);
  // Position bar
  const int barY = y + 92, barX = x + 16, barW = w - 32;
  renderer.drawRect(barX, barY, barW, 10, true);
  if (playing && source.durationSeconds() > 0) {
    const int fill = std::min(barW - 4, (barW - 4) * sec / source.durationSeconds());
    renderer.fillRect(barX + 2, barY + 2, std::max(fill, 4), 6, true);
  }
  // Transport row: |<  >  ||  []  >|  and volume
  const int ty = y + 112;
  int bx = x + 16;
  auto button = [&](bool pressed) {
    renderer.drawRect(bx, ty, 34, 24, true);
    if (pressed) renderer.fillRect(bx + 1, ty + 1, 32, 22, true);
  };
  button(false); renderer.fillRect(bx + 8, ty + 6, 3, 12, true); triangle(renderer, bx + 12, ty + 6, 6, false, true); bx += 38;
  button(playing && !paused); triangle(renderer, bx + 12, ty + 6, 6, true, !(playing && !paused)); bx += 38;
  button(paused); renderer.fillRect(bx + 11, ty + 6, 4, 12, !paused); renderer.fillRect(bx + 19, ty + 6, 4, 12, !paused); bx += 38;
  button(!playing); renderer.fillRect(bx + 11, ty + 6, 12, 12, playing); bx += 38;
  button(false); triangle(renderer, bx + 8, ty + 6, 6, true, true); renderer.fillRect(bx + 22, ty + 6, 3, 12, true); bx += 46;
  // Volume slider
  const int vx = bx, vw = x + w - 16 - vx;
  renderer.drawRect(vx, ty + 9, vw, 6, true);
  renderer.fillRect(vx + (vw - 8) * volume / 100, ty + 4, 8, 16, true);
  renderer.drawText(SMALL_FONT_ID, vx, ty + 24, "VOL");
}

void MusicActivity::drawPlaylist(const int x, const int y, const int w, const int h) const {
  bevel(renderer, x, y, w, h);
  renderer.fillRect(x + 3, y + 3, w - 6, 18, true);
  const bool inFolders = level == FOLDERS;
  renderer.drawText(SMALL_FONT_ID, x + 10, y + 5, inFolders ? tr(STR_MUSIC_FOLDERS) : "PLAYLIST", false);
  const int rowH = 26;
  const int top = y + 26;
  const int rows = std::max(1, (h - 32) / rowH);
  const int count = inFolders ? static_cast<int>(folders.size()) : static_cast<int>(tracks.size());
  const int selected = inFolders ? folderIndex : trackIndex;
  const int first = count > 0 ? (selected / rows) * rows : 0;
  if (count == 0) {
    renderer.drawText(UI_10_FONT_ID, x + 12, top + 6, inFolders ? tr(STR_MUSIC_EMPTY) : tr(STR_MUSIC_NO_TRACKS));
  }
  for (int i = first; i < count && i < first + rows; ++i) {
    const int ry = top + (i - first) * rowH;
    const bool sel = i == selected;
    if (sel) renderer.fillRect(x + 6, ry, w - 12, rowH - 2, true);
    std::string label;
    if (inFolders) {
      const std::string& p = folders[i];
      label = p == MUSIC_ROOT ? std::string("/") : p.substr(p.find_last_of('/') + 1);
    } else {
      label = std::to_string(i + 1) + ". " + trackNames[i];
    }
    const bool isPlaying = !inFolders && i == playingIndex && playing;
    const int tx = x + 12 + (isPlaying ? 14 : 0);
    if (isPlaying) triangle(renderer, x + 12, ry + 6, 5, true, !sel);
    renderer.drawText(UI_10_FONT_ID, tx, ry + 4, renderer.truncatedText(UI_10_FONT_ID, label.c_str(), x + w - 16 - tx).c_str(), !sel);
  }
  if (count > rows) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d/%d", selected / rows + 1, (count + rows - 1) / rows);
    renderer.drawText(SMALL_FONT_ID, x + w - 12 - renderer.getTextWidth(SMALL_FONT_ID, pages), y + h - 18, pages);
  }
}

void MusicActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const int side = 12;
  const int top = metrics.topPadding + 6;
  const int mainH = 150;
  drawMainWindow(side, top, pageWidth - 2 * side, mainH);
  const int listTop = top + mainH + 8;
  const int listH = pageHeight - metrics.buttonHintsHeight - 8 - listTop;
  drawPlaylist(side, listTop, pageWidth - 2 * side, listH);

  if (menuOpen && menu.processRender(renderer, mappedInput)) return;
  const char* confirm = level == FOLDERS ? tr(STR_SELECT) : (playing && trackIndex == playingIndex) ? (paused ? tr(STR_MUSIC_PLAY) : tr(STR_MUSIC_PAUSE)) : tr(STR_MUSIC_PLAY);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastShownSecond = playing ? source.positionSeconds() : -1;
  const bool clean = ++partials >= PARTIALS_BEFORE_CLEAN;
  if (clean) partials = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
