#include "MusicActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MappedInputManager.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "music/MusicPlayer.h"

namespace {
constexpr const char* TAG = "MUSIC";
// La carpeta la pudo haber creado con cualquier mayúscula: en esta SD "/music"
// y "/Music" NO son lo mismo. Se prueban las variantes habituales.
constexpr const char* MUSIC_ROOT_CANDIDATES[] = {"/Music", "/music", "/MUSIC", "/Musica", "/musica"};
std::string musicRoot;
constexpr int MAX_TRACKS = 200;
constexpr int PARTIALS_BEFORE_CLEAN = 12;
constexpr int VOLUME_STEP = 5;

constexpr int SIDE = 16;
constexpr int ROW_H = 44;
constexpr int COVER = 104;
constexpr int CARD_H = COVER + 28;  // carátula + barra de posición debajo

bool endsWithMp3(const char* name) {
  const size_t len = strlen(name);
  if (len < 4) return false;
  const char* ext = name + len - 4;
  return ext[0] == '.' && (ext[1] | 32) == 'm' && (ext[2] | 32) == 'p' && ext[3] == '3';
}

void formatTime(char* out, const size_t size, int seconds) {
  if (seconds < 0) seconds = 0;
  snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// Triangulito de "play" / "esta es la que suena".
void triangle(const GfxRenderer& r, const int x, const int y, const int size) {
  for (int i = 0; i < size; ++i) r.fillRect(x, y + i, size - i, 1, true);
  for (int i = 0; i < size; ++i) r.fillRect(x, y + 2 * size - 1 - i, size - i, 1, true);
}

const std::string& resolveMusicRoot() {
  if (!musicRoot.empty()) return musicRoot;
  for (const char* candidate : MUSIC_ROOT_CANDIDATES) {
    auto dir = Storage.open(candidate);
    if (dir && dir.isDirectory()) {
      musicRoot = candidate;
      LOG_INF(TAG, "carpeta de musica: %s", candidate);
      return musicRoot;
    }
  }
  musicRoot = MUSIC_ROOT_CANDIDATES[0];
  return musicRoot;
}
}  // namespace

void MusicActivity::onEnter() {
  Activity::onEnter();
  scanFolders();
  level = FOLDERS;
  volumeMode = false;
  selected = 0;
  scroll = 0;
  // Si ya hay algo sonando se entra directo a esa carpeta: es lo que uno espera
  // al volver al reproductor.
  if (MUSIC.isActive() && !MUSIC.folderPath().empty()) {
    for (size_t i = 0; i < folders.size(); ++i) {
      if (folders[i] == MUSIC.folderPath()) {
        openFolder(static_cast<int>(i));
        break;
      }
    }
  }
  buildRows();
  forceClean = true;
  requestUpdate();
}

void MusicActivity::scanFolders() {
  folders.clear();
  musicRoot.clear();  // volver a buscarla: pueden haber puesto la tarjeta recién
  const std::string& rootPath = resolveMusicRoot();
  auto root = Storage.open(rootPath.c_str());
  if (!root || !root.isDirectory()) return;
  root.rewindDirectory();
  char name[128];
  bool rootHasMp3 = false;
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (name[0] == '.') continue;
    if (entry.isDirectory()) folders.push_back(rootPath + "/" + name);
    else if (endsWithMp3(name)) rootHasMp3 = true;
  }
  std::sort(folders.begin(), folders.end());
  if (rootHasMp3) folders.insert(folders.begin(), rootPath);
}

void MusicActivity::openFolder(const int index) {
  if (index < 0 || index >= static_cast<int>(folders.size())) return;
  tracks.clear();
  trackNames.clear();
  folderPath = folders[index];
  folderName = folderPath == resolveMusicRoot() ? std::string("/") : folderPath.substr(folderPath.find_last_of('/') + 1);
  auto dir = Storage.open(folderPath.c_str());
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    char name[128];
    for (auto entry = dir.openNextFile(); entry && tracks.size() < MAX_TRACKS; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (entry.isDirectory() || name[0] == '.' || !endsWithMp3(name)) continue;
      tracks.push_back(folderPath + "/" + name);
    }
  }
  std::sort(tracks.begin(), tracks.end());
  for (const std::string& t : tracks) {
    std::string n = t.substr(t.find_last_of('/') + 1);
    n.resize(n.size() - 4);
    trackNames.push_back(n);
  }
  level = PLAYLIST;
  buildRows();
  // Se entra parado en la pista que suena (o en la primera): las acciones
  // quedan justo arriba, a un golpe de palanca.
  selected = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].kind != ROW_TRACK) continue;
    selected = static_cast<int>(i);
    if (MUSIC.folderPath() == folderPath && rows[i].index == MUSIC.index()) break;
    if (MUSIC.folderPath() != folderPath) break;
  }
  scroll = 0;
  forceClean = true;
}

// Una sola lista: primero lo que se puede HACER, después lo que se puede ELEGIR.
void MusicActivity::buildRows() {
  rows.clear();
  if (level == FOLDERS) {
    for (int i = 0; i < static_cast<int>(folders.size()); ++i) rows.push_back({ROW_FOLDER, i, ACT_PLAYPAUSE});
    rows.push_back({ROW_ACTION, 0, ACT_VOLUME});
    return;
  }
  if (MUSIC.isActive()) {
    rows.push_back({ROW_ACTION, 0, ACT_PLAYPAUSE});
    rows.push_back({ROW_ACTION, 0, ACT_NEXT});
    rows.push_back({ROW_ACTION, 0, ACT_PREV});
    rows.push_back({ROW_ACTION, 0, ACT_STOP});
  }
  rows.push_back({ROW_ACTION, 0, ACT_VOLUME});
  rows.push_back({ROW_ACTION, 0, ACT_SHUFFLE});
  rows.push_back({ROW_ACTION, 0, ACT_REPEAT});
  for (int i = 0; i < static_cast<int>(tracks.size()); ++i) rows.push_back({ROW_TRACK, i, ACT_PLAYPAUSE});
}

int MusicActivity::visibleRows(const int listTop) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, (bottom - listTop) / ROW_H);
}

void MusicActivity::clampScroll(const int listTop) {
  const int visible = visibleRows(listTop);
  const int count = static_cast<int>(rows.size());
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + visible) scroll = selected - visible + 1;
  scroll = std::max(0, std::min(scroll, std::max(0, count - visible)));
}

void MusicActivity::moveSelection(const int direction) {
  if (volumeMode) {
    MUSIC.setVolume(MUSIC.volume() + (direction > 0 ? VOLUME_STEP : -VOLUME_STEP));
    requestUpdate();
    return;
  }
  const int count = static_cast<int>(rows.size());
  if (count == 0) return;
  selected = (selected + (direction > 0 ? 1 : count - 1)) % count;
  requestUpdate();
}

void MusicActivity::activate() {
  if (volumeMode) {  // OK termina el ajuste de volumen
    volumeMode = false;
    forceClean = true;
    requestUpdate();
    return;
  }
  if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
  const Row row = rows[selected];
  switch (row.kind) {
    case ROW_FOLDER:
      openFolder(row.index);
      requestUpdate();
      return;
    case ROW_TRACK:
      if (MUSIC.folderPath() == folderPath && MUSIC.index() == row.index && MUSIC.isActive()) {
        MUSIC.togglePause();
      } else {
        MUSIC.playFolder(tracks, trackNames, folderName, folderPath, row.index);
        buildRows();
        // Aparecieron las cuatro filas de mando arriba: la selección se corre
        // con ellas para no quedar parado en otra pista.
        for (size_t i = 0; i < rows.size(); ++i) {
          if (rows[i].kind == ROW_TRACK && rows[i].index == row.index) {
            selected = static_cast<int>(i);
            break;
          }
        }
      }
      forceClean = true;
      requestUpdate();
      return;
    case ROW_ACTION:
      switch (row.action) {
        case ACT_PLAYPAUSE: MUSIC.togglePause(); break;
        case ACT_NEXT: MUSIC.next(false); break;
        case ACT_PREV: MUSIC.previous(); break;
        case ACT_STOP:
          MUSIC.stop();
          buildRows();
          selected = std::min(selected, static_cast<int>(rows.size()) - 1);
          break;
        case ACT_VOLUME: volumeMode = true; break;
        case ACT_SHUFFLE: MUSIC.shuffle = !MUSIC.shuffle; break;
        case ACT_REPEAT: MUSIC.repeat = !MUSIC.repeat; break;
      }
      forceClean = true;
      requestUpdate();
      return;
  }
}

void MusicActivity::goBack() {
  if (volumeMode) {
    volumeMode = false;
    forceClean = true;
    requestUpdate();
    return;
  }
  if (level == PLAYLIST) {
    level = FOLDERS;
    buildRows();
    selected = 0;
    scroll = 0;
    forceClean = true;
    requestUpdate();
    return;
  }
  activityManager.goHome();
}

void MusicActivity::loop() {
  // El contador de tiempo se repinta cada 5 s mientras suena.
  if (MUSIC.isSounding()) {
    const int sec = MUSIC.positionSeconds();
    if (sec / 5 != lastShownSecond / 5) requestUpdate();
  }
  buttonNavigator.onNext([this] { moveSelection(1); });
  buttonNavigator.onPrevious([this] { moveSelection(-1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) goBack();
}

// ── Textos ──────────────────────────────────────────────────────────────────

std::string MusicActivity::rowLabel(const Row& row) const {
  switch (row.kind) {
    case ROW_FOLDER: {
      const std::string& p = folders[row.index];
      return p == resolveMusicRoot() ? std::string("/") : p.substr(p.find_last_of('/') + 1);
    }
    case ROW_TRACK:
      return std::to_string(row.index + 1) + ". " + trackNames[row.index];
    case ROW_ACTION:
      switch (row.action) {
        case ACT_PLAYPAUSE: return MUSIC.isPaused() ? tr(STR_MUSIC_RESUME) : tr(STR_MUSIC_PAUSE);
        case ACT_NEXT: return tr(STR_MUSIC_NEXT);
        case ACT_PREV: return tr(STR_MUSIC_PREV);
        case ACT_STOP: return tr(STR_MUSIC_STOP);
        case ACT_VOLUME: return tr(STR_MUSIC_VOLUME);
        case ACT_SHUFFLE: return tr(STR_MUSIC_SHUFFLE);
        case ACT_REPEAT: return tr(STR_MUSIC_REPEAT);
      }
      return "";
  }
  return "";
}

std::string MusicActivity::rowValue(const Row& row) const {
  if (row.kind != ROW_ACTION) return "";
  char buf[16];
  switch (row.action) {
    case ACT_VOLUME:
      snprintf(buf, sizeof(buf), "%d %%", MUSIC.volume());
      return buf;
    case ACT_SHUFFLE: return MUSIC.shuffle ? tr(STR_MUSIC_ON) : tr(STR_MUSIC_OFF);
    case ACT_REPEAT: return MUSIC.repeat ? tr(STR_MUSIC_ON) : tr(STR_MUSIC_OFF);
    default: return "";
  }
}

const char* MusicActivity::confirmLabel() const {
  if (volumeMode) return tr(STR_MUSIC_VOLUME_DONE);
  if (selected < 0 || selected >= static_cast<int>(rows.size())) return tr(STR_SELECT);
  const Row& row = rows[selected];
  switch (row.kind) {
    case ROW_FOLDER: return tr(STR_MUSIC_OPEN);
    case ROW_TRACK:
      if (MUSIC.isActive() && MUSIC.folderPath() == folderPath && MUSIC.index() == row.index) {
        return MUSIC.isPaused() ? tr(STR_MUSIC_RESUME) : tr(STR_MUSIC_PAUSE);
      }
      return tr(STR_MUSIC_PLAY);
    case ROW_ACTION:
      switch (row.action) {
        case ACT_PLAYPAUSE: return MUSIC.isPaused() ? tr(STR_MUSIC_RESUME) : tr(STR_MUSIC_PAUSE);
        case ACT_NEXT: return tr(STR_MUSIC_NEXT);
        case ACT_PREV: return tr(STR_MUSIC_PREV);
        case ACT_STOP: return tr(STR_MUSIC_STOP);
        case ACT_VOLUME: return tr(STR_MUSIC_VOLUME_ADJUST);
        case ACT_SHUFFLE: return MUSIC.shuffle ? tr(STR_MUSIC_TURN_OFF) : tr(STR_MUSIC_TURN_ON);
        case ACT_REPEAT: return MUSIC.repeat ? tr(STR_MUSIC_TURN_OFF) : tr(STR_MUSIC_TURN_ON);
      }
      return tr(STR_SELECT);
  }
  return tr(STR_SELECT);
}

// ── Dibujo ──────────────────────────────────────────────────────────────────

// Un disco de vinilo dibujado a mano: en 4 grises queda bien y no hay que bajar
// ninguna imagen ni leer la carátula del MP3 (que en esta pantalla no se vería).
void MusicActivity::drawCover(const int x, const int y, const int size) const {
  renderer.fillRoundedRect(x, y, size, size, 10, Color::LightGray);
  renderer.drawRoundedRect(x, y, size, size, 2, 10, true);
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  const int r = size / 2 - 8;
  // El disco: un anillo gris oscuro con dos surcos negros y la etiqueta blanca.
  for (int dy = -r; dy <= r; ++dy) {
    const int half = static_cast<int>(sqrtf(static_cast<float>(r * r - dy * dy)));
    renderer.fillRectDither(cx - half, cy + dy, half * 2, 1, Color::DarkGray);
  }
  for (int ring = r - 6; ring > r / 2; ring -= 7) {
    renderer.drawArc(ring, cx, cy, 1, 1, 1, true);
    renderer.drawArc(ring, cx, cy, -1, 1, 1, true);
    renderer.drawArc(ring, cx, cy, 1, -1, 1, true);
    renderer.drawArc(ring, cx, cy, -1, -1, 1, true);
  }
  const int label = r / 2;
  for (int dy = -label; dy <= label; ++dy) {
    const int half = static_cast<int>(sqrtf(static_cast<float>(label * label - dy * dy)));
    renderer.fillRect(cx - half, cy + dy, half * 2, 1, false);
  }
  renderer.drawArc(label, cx, cy, 1, 1, 2, true);
  renderer.drawArc(label, cx, cy, -1, 1, 2, true);
  renderer.drawArc(label, cx, cy, 1, -1, 2, true);
  renderer.drawArc(label, cx, cy, -1, -1, 2, true);
  renderer.fillRect(cx - 3, cy - 3, 6, 6, true);  // el agujero del centro
}

int MusicActivity::drawNowPlaying(const int x, const int y, const int w) const {
  drawCover(x, y, COVER);

  const int tx = x + COVER + 16;
  const int tw = x + w - tx;
  if (!MUSIC.isActive()) {
    renderer.drawText(UI_12_FONT_ID, tx, y + 10, tr(STR_MUSIC_NOTHING_PLAYING), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, tx, y + 38,
                      renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_HELP_START), tw).c_str());
    renderer.drawText(SMALL_FONT_ID, tx, y + 58,
                      renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_EMPTY_HELP), tw).c_str());
    return CARD_H;
  }

  const std::string title = MUSIC.title();
  renderer.drawText(UI_12_FONT_ID, tx, y + 4, renderer.truncatedText(UI_12_FONT_ID, title.c_str(), tw, EpdFontFamily::BOLD).c_str(),
                    true, EpdFontFamily::BOLD);
  const std::string sub = MUSIC.artist().empty() ? MUSIC.folderName() : MUSIC.artist();
  renderer.drawText(UI_10_FONT_ID, tx, y + 32, renderer.truncatedText(UI_10_FONT_ID, sub.c_str(), tw).c_str());

  // Estado en palabras, que es lo que hay que poder leer de un vistazo.
  const char* state = MUSIC.isPaused() ? tr(STR_MUSIC_PAUSED) : tr(STR_MUSIC_PLAYING);
  renderer.drawText(UI_10_FONT_ID, tx, y + 58, state, true, EpdFontFamily::BOLD);

  char now[16];
  char total[16];
  char clock[40];
  formatTime(now, sizeof(now), MUSIC.positionSeconds());
  formatTime(total, sizeof(total), MUSIC.durationSeconds());
  snprintf(clock, sizeof(clock), "%s / %s", now, total);
  renderer.drawText(UI_10_FONT_ID, x + w - renderer.getTextWidth(UI_10_FONT_ID, clock), y + 58, clock);

  char pos[24];
  snprintf(pos, sizeof(pos), "%d / %d", MUSIC.index() + 1, MUSIC.count());
  renderer.drawText(SMALL_FONT_ID, tx, y + 84, pos);

  // Barra de posición, ancho completo debajo de la carátula.
  const int barY = y + COVER + 10;
  renderer.drawRoundedRect(x, barY, w, 14, 2, 7, true);
  const int span = MUSIC.durationSeconds();
  const int fill = span > 0 ? std::min(w - 4, (w - 4) * MUSIC.positionSeconds() / span) : 0;
  if (fill > 0) renderer.fillRoundedRect(x + 2, barY + 2, fill, 10, 5, Color::Black);
  return CARD_H;
}

void MusicActivity::drawList(const int x, const int y, const int w, const int h) {
  clampScroll(y);
  int top = y;
  // Sin carpetas la lista igual trae la fila de volumen, así que el cartel de
  // "no hay MP3" va ARRIBA de la lista, no en lugar de ella: si no, con la
  // tarjeta vacía la pantalla no explicaba nada.
  if ((level == FOLDERS && folders.empty()) || (level == PLAYLIST && tracks.empty())) {
    renderer.drawText(UI_12_FONT_ID, x, top, tr(STR_MUSIC_EMPTY), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, x, top + 26,
                      renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_EMPTY_HELP), w).c_str());
    top += 56;
  }
  const int visible =
      std::min(std::min(visibleRows(top), (y + h - top) / ROW_H), static_cast<int>(rows.size()) - scroll);
  if (rows.empty()) return;
  for (int i = 0; i < visible; ++i) {
    const int idx = scroll + i;
    const Row& row = rows[idx];
    const int ry = top + i * ROW_H;
    const bool sel = idx == selected;
    // Raya fina donde terminan las acciones y empiezan las pistas: son dos
    // cosas distintas y en una lista corrida se mezclaban.
    if (idx > 0 && row.kind == ROW_TRACK && rows[idx - 1].kind == ROW_ACTION) {
      renderer.drawLine(x - 6, ry - 3, x + w + 6, ry - 3, true);
    }
    if (sel) drawSelectionRow(renderer, x - 6, ry, w + 12, ROW_H - 6, 10);

    int textX = x + 6;
    // La pista que suena lleva su triangulito; las acciones, un punto.
    if (row.kind == ROW_TRACK && MUSIC.isActive() && MUSIC.folderPath() == folderPath && MUSIC.index() == row.index) {
      triangle(renderer, textX, ry + 10, 8);
      textX += 18;
    } else if (row.kind == ROW_ACTION) {
      renderer.fillRoundedRect(textX + 2, ry + ROW_H / 2 - 9, 6, 6, 3, Color::Black);
      textX += 18;
    }

    const std::string value = rowValue(row);
    const int valueW = value.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, value.c_str()) + 12;
    const std::string label = rowLabel(row);
    const int labelW = std::max(20, x + w - textX - valueW);
    const EpdFontFamily::Style style = row.kind == ROW_ACTION ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_12_FONT_ID, textX, ry + 8,
                      renderer.truncatedText(UI_12_FONT_ID, label.c_str(), labelW, style).c_str(), SELECTION_INK, style);
    if (!value.empty()) {
      renderer.drawText(UI_10_FONT_ID, x + w - (valueW - 12), ry + 11, value.c_str(), SELECTION_INK);
    }
  }

  if (static_cast<int>(rows.size()) > visible) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%d/%d", selected + 1, static_cast<int>(rows.size()));
    const int pw = renderer.getTextWidth(SMALL_FONT_ID, pager);
    renderer.drawText(SMALL_FONT_ID, x + w - pw, y - 20, pager);
  }
}

void MusicActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  buildRows();
  if (selected >= static_cast<int>(rows.size())) selected = std::max(0, static_cast<int>(rows.size()) - 1);

  const int x = SIDE;
  const int w = pageWidth - 2 * SIDE;
  int y = metrics.topPadding + 6;

  // Encabezado: dónde estoy (Música / nombre de la carpeta).
  const std::string header = level == FOLDERS ? std::string(tr(STR_HUB_MUSIC)) : folderName;
  renderer.drawText(UI_12_FONT_ID, x, y, renderer.truncatedText(UI_12_FONT_ID, header.c_str(), w - 60, EpdFontFamily::BOLD).c_str(),
                    true, EpdFontFamily::BOLD);
  y += 28;
  renderer.drawLine(x, y, x + w, y, true);
  y += 12;

  y += drawNowPlaying(x, y, w) + 14;

  // Una línea que dice qué está haciendo la palanca AHORA. Es la instrucción
  // que faltaba: sin esto el modo volumen parecía que se colgó.
  const char* help = volumeMode ? tr(STR_MUSIC_HELP_VOLUME)
                     : level == FOLDERS ? tr(STR_MUSIC_HELP_FOLDERS)
                                        : tr(STR_MUSIC_HELP_LIST);
  renderer.drawText(SMALL_FONT_ID, x, y, renderer.truncatedText(SMALL_FONT_ID, help, w - 60).c_str());
  y += 24;

  drawList(x, y, w, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - y);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel(),
                                            volumeMode ? tr(STR_MUSIC_VOL_UP) : tr(STR_DIR_UP),
                                            volumeMode ? tr(STR_MUSIC_VOL_DOWN) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastShownSecond = MUSIC.positionSeconds();
  const bool clean = forceClean || ++partials >= PARTIALS_BEFORE_CLEAN;
  if (clean) {
    partials = 0;
    forceClean = false;
  }
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
