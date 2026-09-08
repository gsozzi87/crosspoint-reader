#include "MusicActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

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
constexpr unsigned long LEAVE_HOLD_MS = 1000;  // Atrás mantenido = salir
constexpr int COUNTER_TICK_S = 5;              // refresco parcial del contador
constexpr int PARTIALS_BEFORE_CLEAN = 12;      // regla del panel: refresco limpio cada 10-15 parciales
constexpr int VOLUME_STEP = 5;

constexpr int SIDE = 12;         // margen lateral
constexpr int BAND_GAP = 10;     // aire entre bandas
constexpr int NOW_H = 118;       // banda "sonando"
constexpr int CTRL_H = 52;       // banda de mandos
constexpr int VOL_H = 58;        // banda de volumen
constexpr int LIST_HEADER_H = 26;
constexpr int LIST_ROW_H = 34;

bool endsWithMp3(const char* name) {
  const size_t len = strlen(name);
  if (len < 4) return false;
  const char* ext = name + len - 4;
  return (ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'p' || ext[2] == 'P') && ext[3] == '3');
}

void triangle(const GfxRenderer& r, int x, int y, int size, bool right, bool ink) {
  for (int i = 0; i < size; ++i) {
    const int len = right ? size - i : i + 1;
    const int sx = right ? x : x + size - len;
    r.fillRect(sx, y + i, len, 1, ink);
    r.fillRect(sx, y + 2 * size - 1 - i, len, 1, ink);
  }
}

// Marco de la zona con el foco: 3 px, para que se vea de lejos cuál manda la
// palanca. Las zonas sin foco no llevan marco (menos tinta, menos fantasma).
void focusFrame(const GfxRenderer& r, int x, int y, int w, int h, bool focused) {
  if (focused) r.drawRect(x, y, w, h, 3, true);
}

void centeredText(const GfxRenderer& r, int fontId, int x, int w, int y, const char* text, bool ink,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int tw = r.getTextWidth(fontId, text, style);
  r.drawText(fontId, x + (w - tw) / 2, y, text, ink, style);
}

void formatTime(char* out, size_t size, int seconds) {
  if (seconds < 0) seconds = 0;
  snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}
}  // namespace

void MusicActivity::onEnter() {
  Activity::onEnter();
  volume = HUB_STORE.musicVolume >= 0 && HUB_STORE.musicVolume <= 100 ? HUB_STORE.musicVolume : 70;
  scanFolders();
  level = FOLDERS;
  zone = ZONE_LIST;
  controlIndex = CTRL_PLAY;
  requestUpdate();
}

void MusicActivity::onExit() {
  Activity::onExit();
  stop();
  audio.end();
}

int MusicActivity::listCount() const {
  return level == FOLDERS ? static_cast<int>(folders.size()) : static_cast<int>(tracks.size());
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

// Arranca una pista de la carpeta que se está mirando: primero se copia esa
// carpeta a la lista que suena, así explorar otra no toca lo que se reproduce.
bool MusicActivity::playSelected(const int index) {
  if (index < 0 || index >= static_cast<int>(tracks.size())) return false;
  playTracks = tracks;
  playTrackNames = trackNames;
  playFolderName = folderName;
  playFolderIndex = folderIndex;
  return play(index);
}

bool MusicActivity::play(const int index) {
  if (index < 0 || index >= static_cast<int>(playTracks.size())) return false;
  audio.stop();
  playing = false;
  paused = false;
  if (!source.open(playTracks[index])) {
    LOG_ERR(TAG, "cannot open %s", playTracks[index].c_str());
    playingIndex = -1;
    return false;
  }
  // Todo camino de error cierra la fuente: si no, quedan colgados el handle del
  // MP3 y los buffers del decodificador, y `playing` en true hacía que el loop
  // entrara en el ciclo "terminó la pista -> next()".
  if (!audio.begin()) {
    source.close();
    playingIndex = -1;
    return false;
  }
  audio.setVolume(volume);
  if (!audio.play(source.wavSource(), false)) {
    source.close();
    playingIndex = -1;
    return false;
  }
  playingIndex = index;
  // Queda en el log (útil cuando una pista corta o salta sola).
  LOG_INF(TAG, "suena %d/%u %s", index + 1, static_cast<unsigned>(playTracks.size()),
          index < static_cast<int>(playTrackNames.size()) ? playTrackNames[index].c_str() : "");
  if (folderIndex == playFolderIndex) trackIndex = index;  // la lista a la vista es la que suena
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
  playFolderIndex = -1;
  playTracks.clear();
  playTrackNames.clear();
  playFolderName.clear();
}

// The SDK has no pause: stopping and replaying restarts the track, so pause
// mutes and keeps the stream (the decoder keeps feeding silence-free PCM).
void MusicActivity::togglePause() {
  if (!playing) return;
  paused = !paused;
  audio.setVolume(paused ? 0 : volume);
  requestUpdate();
}

// Con algo cargado alterna pausa; con el reproductor parado arranca la pista
// que está elegida en la lista (si no, OK no hacía nada y no se entendía).
void MusicActivity::playPause() {
  if (playing) {
    togglePause();
    return;
  }
  if (level == PLAYLIST) playSelected(trackIndex);
}

// Siempre sobre la carpeta que suena, no sobre la que se está explorando.
void MusicActivity::next(const bool fromEnd) {
  if (playTracks.empty()) return;
  int idx;
  if (shuffle && playTracks.size() > 1) {
    do {
      idx = static_cast<int>(esp_random() % playTracks.size());
    } while (idx == playingIndex);
  } else {
    idx = playingIndex + 1;
    if (idx >= static_cast<int>(playTracks.size())) {
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
  if (playTracks.empty()) return;
  int idx = playingIndex - 1;
  if (idx < 0) idx = static_cast<int>(playTracks.size()) - 1;
  play(idx);
}

// El volumen es uno solo para todo el aparato (música, voz de Piper y pitidos),
// así que se guarda en HubStore apenas cambia.
void MusicActivity::setVolume(const int value) {
  const int clamped = std::max(0, std::min(100, value));
  if (clamped == volume) return;
  volume = clamped;
  audio.setVolume(paused ? 0 : volume);
  HUB_STORE.musicVolume = volume;
  HUB_STORE.saveToFile();
  requestUpdate();
}

void MusicActivity::step(const int direction) {
  switch (zone) {
    case ZONE_LIST: {
      const int count = listCount();
      if (count <= 0) break;
      int& index = level == FOLDERS ? folderIndex : trackIndex;
      index = direction > 0 ? ButtonNavigator::nextIndex(index, count) : ButtonNavigator::previousIndex(index, count);
      break;
    }
    case ZONE_TRANSPORT:
      controlIndex = direction > 0 ? ButtonNavigator::nextIndex(controlIndex, CTRL_COUNT)
                                   : ButtonNavigator::previousIndex(controlIndex, CTRL_COUNT);
      break;
    case ZONE_VOLUME:
      setVolume(volume + (direction > 0 ? VOLUME_STEP : -VOLUME_STEP));
      return;  // setVolume ya pidió el repintado (o no hubo cambio)
    default:
      break;
  }
  requestUpdate();
}

void MusicActivity::activate() {
  switch (zone) {
    case ZONE_LIST:
      if (level == FOLDERS) {
        openFolder(folderIndex);
      } else if (isSelectedPlaying()) {
        togglePause();
      } else {
        playSelected(trackIndex);
      }
      return;
    case ZONE_TRANSPORT:
      switch (controlIndex) {
        case CTRL_PLAY: playPause(); break;
        case CTRL_PREV: previous(); break;
        case CTRL_NEXT: next(false); break;
        case CTRL_STOP: stop(); break;
        case CTRL_SHUFFLE: shuffle = !shuffle; break;
        case CTRL_REPEAT: repeat = !repeat; break;
        default: break;
      }
      requestUpdate();
      return;
    case ZONE_VOLUME:
      playPause();
      return;
    default:
      return;
  }
}

void MusicActivity::leaveOrExit() {
  if (level == PLAYLIST) {
    level = FOLDERS;  // la música sigue sonando; la lista vuelve a las carpetas
    zone = ZONE_LIST;
    requestUpdate();
    return;
  }
  activityManager.goHome();
}

void MusicActivity::loop() {
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

  // Atrás mantenido sale; el corto cambia de zona (wasLongPressed se come la
  // suelta, así que nunca disparan los dos).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, LEAVE_HOLD_MS)) {
    leaveOrExit();
    return;
  }

  buttonNavigator.onNext([this] { step(1); });
  buttonNavigator.onPrevious([this] { step(-1); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    zone = static_cast<Zone>((zone + 1) % ZONE_COUNT);
    requestUpdate();
  }
}

const char* MusicActivity::zoneLabel(const Zone z) const {
  switch (z) {
    case ZONE_TRANSPORT: return tr(STR_MUSIC_ZONE_CONTROLS);
    case ZONE_VOLUME: return tr(STR_MUSIC_VOLUME);
    default: return tr(STR_MUSIC_ZONE_LIST);
  }
}

const char* MusicActivity::controlLabel(const int control) const {
  switch (control) {
    case CTRL_PLAY: return playing && !paused ? tr(STR_MUSIC_PAUSE) : tr(STR_MUSIC_PLAY);
    case CTRL_PREV: return tr(STR_MUSIC_PREV);
    case CTRL_NEXT: return tr(STR_MUSIC_NEXT);
    case CTRL_STOP: return tr(STR_MUSIC_STOP);
    case CTRL_SHUFFLE: return tr(STR_MUSIC_SHUFFLE);
    case CTRL_REPEAT: return tr(STR_MUSIC_REPEAT);
    default: return "";
  }
}

const char* MusicActivity::confirmLabel() const {
  switch (zone) {
    case ZONE_LIST:
      if (level == FOLDERS) return tr(STR_MUSIC_OPEN);
      if (isSelectedPlaying()) return paused ? tr(STR_MUSIC_PLAY) : tr(STR_MUSIC_PAUSE);
      return tr(STR_MUSIC_PLAY);
    case ZONE_TRANSPORT:
      return controlLabel(controlIndex);
    case ZONE_VOLUME:
      return playing && !paused ? tr(STR_MUSIC_PAUSE) : tr(STR_MUSIC_PLAY);
    default:
      return tr(STR_SELECT);
  }
}

// Lo que está sonando: título, artista, tiempo y barra de posición. No lleva
// foco (no hay nada que elegir acá), así que no lleva marco.
int MusicActivity::drawNowPlaying(const int x, const int y, const int w) const {
  const bool named = playing && playingIndex >= 0 && playingIndex < static_cast<int>(playTrackNames.size());
  const std::string title = playing ? (!source.title().empty() ? source.title()
                                       : named                 ? playTrackNames[playingIndex]
                                                               : std::string())
                                    : std::string(tr(STR_MUSIC_NOTHING_PLAYING));
  renderer.drawText(UI_12_FONT_ID, x, y, renderer.truncatedText(UI_12_FONT_ID, title.c_str(), w, EpdFontFamily::BOLD).c_str(),
                    true, EpdFontFamily::BOLD);

  const std::string sub = playing ? (source.artist().empty() ? playFolderName : source.artist()) : folderName;
  if (!sub.empty()) {
    renderer.drawText(UI_10_FONT_ID, x, y + 26, renderer.truncatedText(UI_10_FONT_ID, sub.c_str(), w).c_str());
  }

  char left[48];
  if (playing) {
    char now[16];
    char total[16];
    formatTime(now, sizeof(now), source.positionSeconds());
    formatTime(total, sizeof(total), source.durationSeconds());
    snprintf(left, sizeof(left), "%s / %s", now, total);
  } else {
    snprintf(left, sizeof(left), "--:-- / --:--");
  }
  renderer.drawText(UI_12_FONT_ID, x, y + 48, left, true, EpdFontFamily::BOLD);

  const char* state = !playing ? tr(STR_MUSIC_STOPPED) : (paused ? tr(STR_MUSIC_PAUSED) : tr(STR_MUSIC_PLAYING));
  renderer.drawText(UI_10_FONT_ID, x + w - renderer.getTextWidth(UI_10_FONT_ID, state), y + 50, state);

  // Barra de posición
  const int barY = y + 74;
  renderer.drawRect(x, barY, w, 14, true);
  if (playing) {
    const int total = source.durationSeconds();
    const int fill = total > 0 ? std::min(w - 4, (w - 4) * source.positionSeconds() / total) : 0;
    renderer.fillRect(x + 2, barY + 2, std::max(fill, 2), 10, true);
  }

  // Ayuda fija: en este aparato Atrás hace dos cosas y hay que decirlo.
  renderer.drawText(SMALL_FONT_ID, x, y + 96,
                    renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_BACK_HELP), w).c_str());
  return NOW_H;
}

// Los seis mandos, que ahora se eligen de verdad con la palanca y se activan
// con OK (antes eran un dibujo: no había forma de tocarlos).
int MusicActivity::drawControls(const int x, const int y, const int w) const {
  const int gap = 6;
  const int bw = (w - gap * (CTRL_COUNT - 1)) / CTRL_COUNT;
  const int bh = CTRL_H - 4;
  const bool sounding = playing && !paused;
  for (int i = 0; i < CTRL_COUNT; ++i) {
    const int bx = x + i * (bw + gap);
    const bool focused = zone == ZONE_TRANSPORT && controlIndex == i;
    renderer.drawRect(bx, y, bw, bh, focused ? 3 : 1, true);
    const int cx = bx + bw / 2;
    const int iconY = y + 8;
    switch (i) {
      case CTRL_PLAY:
        if (sounding) {  // pausa
          renderer.fillRect(cx - 7, iconY, 5, 16, true);
          renderer.fillRect(cx + 2, iconY, 5, 16, true);
        } else {
          triangle(renderer, cx - 5, iconY, 8, true, true);
        }
        break;
      case CTRL_PREV:
        renderer.fillRect(cx - 8, iconY, 3, 16, true);
        triangle(renderer, cx - 3, iconY, 8, false, true);
        break;
      case CTRL_NEXT:
        triangle(renderer, cx - 8, iconY, 8, true, true);
        renderer.fillRect(cx + 5, iconY, 3, 16, true);
        break;
      case CTRL_STOP:
        renderer.fillRect(cx - 7, iconY + 1, 14, 14, true);
        break;
      case CTRL_SHUFFLE:  // dos líneas que se cruzan
        renderer.drawLine(cx - 9, iconY + 1, cx + 9, iconY + 14, 2, true);
        renderer.drawLine(cx - 9, iconY + 14, cx + 9, iconY + 1, 2, true);
        break;
      case CTRL_REPEAT:  // lazo
        renderer.drawRect(cx - 9, iconY + 2, 18, 12, 2, true);
        break;
      default:
        break;
    }
    centeredText(renderer, SMALL_FONT_ID, bx, bw, y + bh - 16,
                 renderer.truncatedText(SMALL_FONT_ID, controlLabel(i), bw - 6).c_str(), true);
    // Estado de los interruptores: barra llena arriba cuando están activados
    // (abajo se pisaba con la palabra del mando).
    const bool on = (i == CTRL_SHUFFLE && shuffle) || (i == CTRL_REPEAT && repeat);
    if (on) renderer.fillRect(bx + 4, y + 3, bw - 8, 3, true);
  }
  return CTRL_H;
}

int MusicActivity::drawVolume(const int x, const int y, const int w) const {
  focusFrame(renderer, x - 6, y - 4, w + 12, VOL_H, zone == ZONE_VOLUME);
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_MUSIC_VOLUME));
  char value[16];
  snprintf(value, sizeof(value), "%d %%", volume);
  renderer.drawText(UI_10_FONT_ID, x + w - renderer.getTextWidth(UI_10_FONT_ID, value), y, value);
  const int barY = y + 20;
  renderer.drawRect(x, barY, w, 16, true);
  const int volFill = (w - 4) * volume / 100;
  if (volFill > 0) renderer.fillRect(x + 2, barY + 2, volFill, 12, true);
  renderer.drawText(SMALL_FONT_ID, x, y + 40,
                    renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_VOLUME_HELP), w).c_str());
  return VOL_H;
}

void MusicActivity::drawList(const int x, const int y, const int w, const int h) const {
  focusFrame(renderer, x - 6, y - 4, w + 12, h + 8, zone == ZONE_LIST);
  const bool inFolders = level == FOLDERS;
  renderer.fillRect(x, y, w, LIST_HEADER_H, true);
  const std::string header = inFolders ? std::string(tr(STR_MUSIC_FOLDERS)) : folderName;
  renderer.drawText(UI_10_FONT_ID, x + 8, y + 5,
                    renderer.truncatedText(UI_10_FONT_ID, header.c_str(), w - 90).c_str(), false);

  const int top = y + LIST_HEADER_H + 4;
  const int rows = std::max(1, (h - LIST_HEADER_H - 6) / LIST_ROW_H);
  const int count = inFolders ? static_cast<int>(folders.size()) : static_cast<int>(tracks.size());
  const int selected = inFolders ? folderIndex : trackIndex;
  const int first = count > 0 ? (selected / rows) * rows : 0;

  if (count == 0) {
    renderer.drawText(UI_10_FONT_ID, x + 8, top + 6, inFolders ? tr(STR_MUSIC_EMPTY) : tr(STR_MUSIC_NO_TRACKS));
    renderer.drawText(SMALL_FONT_ID, x + 8, top + 30,
                      renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_EMPTY_HELP), w - 16).c_str());
    return;
  }

  for (int i = first; i < count && i < first + rows; ++i) {
    const int ry = top + (i - first) * LIST_ROW_H;
    const bool sel = i == selected;
    if (sel) renderer.fillRect(x + 2, ry, w - 4, LIST_ROW_H - 3, true);
    std::string label;
    if (inFolders) {
      const std::string& p = folders[i];
      label = p == MUSIC_ROOT ? std::string("/") : p.substr(p.find_last_of('/') + 1);
    } else {
      label = std::to_string(i + 1) + ". " + trackNames[i];
    }
    const bool isPlaying = !inFolders && playing && folderIndex == playFolderIndex && i == playingIndex;
    const int tx = x + 10 + (isPlaying ? 18 : 0);
    if (isPlaying) triangle(renderer, x + 10, ry + 8, 6, true, !sel);
    renderer.drawText(UI_12_FONT_ID, tx, ry + 6,
                      renderer.truncatedText(UI_12_FONT_ID, label.c_str(), x + w - 10 - tx).c_str(), !sel);
  }
  if (count > rows) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d/%d", selected / rows + 1, (count + rows - 1) / rows);
    renderer.drawText(UI_10_FONT_ID, x + w - 8 - renderer.getTextWidth(UI_10_FONT_ID, pages), y + 5, pages, false);
  }
}

void MusicActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int x = SIDE + 6;               // el marco de foco vive en los 6 px de afuera
  const int w = pageWidth - 2 * x;
  int y = metrics.topPadding + 10;

  // Título de la pantalla
  renderer.drawText(UI_12_FONT_ID, x, y, tr(STR_HUB_MUSIC), true, EpdFontFamily::BOLD);
  renderer.drawLine(x, y + 24, x + w, y + 24, true);
  y += 34;

  y += drawNowPlaying(x, y, w) + BAND_GAP;
  y += drawControls(x, y, w) + BAND_GAP;
  y += drawVolume(x, y, w) + BAND_GAP;

  const int listH = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - y - 4;
  if (listH > LIST_HEADER_H + LIST_ROW_H) drawList(x, y, w, listH);

  const Zone nextZone = static_cast<Zone>((zone + 1) % ZONE_COUNT);
  const auto labels = mappedInput.mapLabels(zoneLabel(nextZone), confirmLabel(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastShownSecond = playing ? source.positionSeconds() : -1;
  const bool clean = ++partials >= PARTIALS_BEFORE_CLEAN;
  if (clean) partials = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
