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
#include "HubStore.h"
#include "components/Selection.h"
#include "components/SevenSegment.h"
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

// Medidas de la "skin". La pantalla es de 480x800, así que el Winamp va
// apilado: título, visor, posición, botonera, volumen y la lista abajo.
constexpr int SIDE = 10;
constexpr int ROW_H = 34;        // fila de la lista de pistas
// Borde del panel de la lista -> texto. Tiene que ser >= 22 px: el resalte pone
// franjas tramadas de 16 px por dentro del marco de la fila y la regla del
// rediseño es que NUNCA hay letras sobre trama.
constexpr int LIST_PAD = 24;
constexpr int LIST_HEADER_H = 28;  // cabecera de la lista + su regla de 1 px
constexpr int LIST_META_GAP = 12;  // título de la pista -> duración de la derecha
constexpr int LIST_TAIL = 4;       // aire entre la última fila y el marco del panel
constexpr int TITLEBAR_H = 30;
constexpr int DISPLAY_H = 160;  // tres renglones debajo del contador, sin que el último toque el marco
constexpr int POS_H = 20;
constexpr int TRANSPORT_H = 54;
constexpr int VOL_H = 30;
constexpr int GAP = 8;

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

// Triangulito lleno de alto 2*size y ancho size. `right=false` lo da vuelta.
//
// ESTE HELPER NUNCA DIBUJO UN "PLAY". Con right=true hacia `len = size - i`, o
// sea que la fila de ARRIBA era la mas ancha y la del medio la mas angosta: eso
// no es un triangulo apuntando a la derecha, es una cuna con una muesca. Por eso
// en la pantalla el boton de reproducir se veia como una "K" y el de siguiente
// apuntaba para el lado equivocado. Un triangulo que apunta a la derecha es al
// reves: angosto arriba y ancho en el medio, que es donde esta la punta.
void triangle(const GfxRenderer& r, const int x, const int y, const int size, const bool right = true,
              const bool black = true) {
  for (int i = 0; i < size; ++i) {
    const int len = i + 1;                             // crece hacia el medio
    const int sx = right ? x : x + size - len;         // la punta cae del lado que toca
    r.fillRect(sx, y + i, len, 1, black);              // mitad de arriba
    r.fillRect(sx, y + 2 * size - 1 - i, len, 1, black);  // espejada abajo
  }
}

// Recuadro con biselado a lo Winamp: marco negro y una luz interior clara
// arriba/izquierda. Es lo que hace que un rectángulo parezca un botón.
void bevel(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int radius,
           const bool pressed) {
  r.fillRoundedRect(x, y, w, h, radius, pressed ? Color::LightGray : Color::White);
  r.drawRoundedRect(x, y, w, h, pressed ? 3 : 2, radius, true);
  if (!pressed) {  // brillo de arriba, que da el relieve
    r.fillRect(x + radius, y + 3, w - 2 * radius, 1, true);
  }
}

// Marco hundido del visor y de las barras: doble línea, la de adentro más
// clara. Winamp puro.
void inset(const GfxRenderer& r, const int x, const int y, const int w, const int h) {
  r.drawRect(x, y, w, h, 2, true);
  r.fillRectDither(x + 2, y + 2, w - 4, 1, Color::DarkGray);
  r.fillRectDither(x + 2, y + 2, 1, h - 4, Color::DarkGray);
}

// Duración de un MP3 sin levantar el decodificador: se saltea la etiqueta ID3v2,
// se lee la cabecera del primer cuadro y, si el archivo trae Xing/Info (los VBR),
// se usa la cuenta de cuadros; si no, tamaño sobre bitrate, que es exacto en CBR.
// Es media lectura de sector por archivo y solo se hace con las filas que se ven.
// Devuelve 0 cuando el archivo no permite calcularla.
int mp3DurationSeconds(const std::string& path) {
  static const int V1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const int V2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  static const int RATES[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return 0;
  const size_t fileSize = f.size();
  size_t audioStart = 0;
  uint8_t head[10];
  if (f.read(head, sizeof(head)) == static_cast<int>(sizeof(head)) && memcmp(head, "ID3", 3) == 0) {
    const size_t tag = (static_cast<size_t>(head[6] & 0x7F) << 21) | (static_cast<size_t>(head[7] & 0x7F) << 14) |
                       (static_cast<size_t>(head[8] & 0x7F) << 7) | static_cast<size_t>(head[9] & 0x7F);
    audioStart = 10 + tag + ((head[5] & 0x10) ? 10 : 0);  // pie de la etiqueta, si lo hay
  }
  if (audioStart + 4 >= fileSize || !f.seek(audioStart)) {
    f.close();
    return 0;
  }
  // 512 bytes alcanzan de sobra (el primer sync suele estar en el byte 0 y la
  // cabecera Xing 36 más adelante) y la tarea de dibujo tiene 8 KB de pila.
  uint8_t buf[512];
  const int got = f.read(buf, sizeof(buf));
  f.close();
  if (got < 4) return 0;

  for (int i = 0; i + 4 <= got; ++i) {
    if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
    const int version = (buf[i + 1] >> 3) & 3;  // 0 = MPEG2.5, 2 = MPEG2, 3 = MPEG1
    const int layer = (buf[i + 1] >> 1) & 3;    // 1 = Layer III
    const int bitrateIdx = (buf[i + 2] >> 4) & 0x0F;
    const int rateIdx = (buf[i + 2] >> 2) & 3;
    if (version == 1 || layer != 1 || bitrateIdx == 0 || bitrateIdx == 15 || rateIdx == 3) continue;
    const int rate = RATES[version][rateIdx];
    const int kbps = version == 3 ? V1L3[bitrateIdx] : V2L3[bitrateIdx];
    if (rate <= 0 || kbps <= 0) continue;
    const int perFrame = version == 3 ? 1152 : 576;

    // Cabecera Xing/Info: va en el hueco del primer cuadro, a una distancia que
    // depende de la versión y de si es mono.
    const bool mono = ((buf[i + 3] >> 6) & 3) == 3;
    const int xing = i + 4 + (version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (xing + 12 <= got && (memcmp(buf + xing, "Xing", 4) == 0 || memcmp(buf + xing, "Info", 4) == 0)) {
      const uint32_t flags = (static_cast<uint32_t>(buf[xing + 4]) << 24) | (buf[xing + 5] << 16) |
                             (buf[xing + 6] << 8) | buf[xing + 7];
      if (flags & 1) {
        const uint32_t frames = (static_cast<uint32_t>(buf[xing + 8]) << 24) | (buf[xing + 9] << 16) |
                                (buf[xing + 10] << 8) | buf[xing + 11];
        if (frames > 0) return static_cast<int>(static_cast<uint64_t>(frames) * perFrame / rate);
      }
    }
    // El sync está en `audioStart + i`: el audio de verdad empieza ahí.
    const size_t audioBytes = fileSize - audioStart - static_cast<size_t>(i);
    return static_cast<int>(static_cast<uint64_t>(audioBytes) * 8 / (static_cast<uint64_t>(kbps) * 1000));
  }
  return 0;
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
  selected = TRANSPORT_COUNT + 1;
  scroll = TRANSPORT_COUNT + 1;
  // A que carpeta entrar, en orden: la que esta sonando, la ultima que se abrio
  // y, si hay una sola, esa. Antes habia que elegirla a mano CADA vez y el
  // reproductor no se acordaba de nada, ni siquiera cuando habia una carpeta y
  // nada mas.
  const std::string wanted =
      MUSIC.isActive() && !MUSIC.folderPath().empty() ? MUSIC.folderPath() : HUB_STORE.musicFolder;
  bool opened = false;
  if (!wanted.empty()) {
    for (size_t i = 0; i < folders.size(); ++i) {
      if (folders[i] == wanted) {
        openFolder(static_cast<int>(i));
        opened = true;
        break;
      }
    }
  }
  if (!opened && folders.size() == 1) openFolder(0);
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
  if (HUB_STORE.musicFolder != folderPath) {
    HUB_STORE.musicFolder = folderPath;
    HUB_STORE.saveToFile();
  }
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
  trackSeconds.assign(tracks.size(), -1);  // se miden a medida que las filas se ven
  level = PLAYLIST;
  buildRows();
  // Se entra parado en la pista que suena (o en la primera): la botonera queda
  // justo arriba, a un golpe de palanca.
  selected = TRANSPORT_COUNT + 1;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].kind != ROW_TRACK) continue;
    selected = static_cast<int>(i);
    if (MUSIC.folderPath() == folderPath && rows[i].index == MUSIC.index()) break;
    if (MUSIC.folderPath() != folderPath) break;
  }
  scroll = TRANSPORT_COUNT + 1;
  forceClean = true;
}

// Una sola lista por dentro: los seis botones de transporte, el volumen y
// después lo que se puede elegir (carpetas o pistas). Los siete primeros los
// dibuja la skin arriba; del octavo en adelante es la lista de abajo.
void MusicActivity::buildRows() {
  rows.clear();
  rows.push_back({ROW_ACTION, 0, ACT_PREV});
  rows.push_back({ROW_ACTION, 0, ACT_PLAYPAUSE});
  rows.push_back({ROW_ACTION, 0, ACT_NEXT});
  rows.push_back({ROW_ACTION, 0, ACT_STOP});
  rows.push_back({ROW_ACTION, 0, ACT_SHUFFLE});
  rows.push_back({ROW_ACTION, 0, ACT_REPEAT});
  rows.push_back({ROW_ACTION, 0, ACT_VOLUME});
  if (level == FOLDERS) {
    for (int i = 0; i < static_cast<int>(folders.size()); ++i) rows.push_back({ROW_FOLDER, i, ACT_PREV});
  } else {
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) rows.push_back({ROW_TRACK, i, ACT_PREV});
  }
}

// Cuántas filas entran de verdad. La cuenta tiene que ser LA MISMA que la del
// bucle de dibujo (`listH / ROW_H`): si acá sale una de más, `clampScroll` deja
// la fila elegida justo abajo del borde del panel y parece que se perdió.
int MusicActivity::visibleRows(const int listTop) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, (bottom - LIST_TAIL - listTop) / ROW_H);
}

// `scroll` es la PRIMERA fila de la lista que se ve, siempre de la parte de
// abajo (la botonera y el volumen viven arriba y no se desplazan).
void MusicActivity::clampScroll(const int listTop) {
  const int first = TRANSPORT_COUNT + 1;
  const int count = static_cast<int>(rows.size());
  const int visible = visibleRows(listTop);
  if (selected < first) {  // el foco está en la skin: la lista se queda al principio
    scroll = first;
    return;
  }
  if (scroll < first) scroll = first;
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + visible) scroll = selected - visible + 1;
  scroll = std::max(first, std::min(scroll, std::max(first, count - visible)));
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
        case ACT_PLAYPAUSE:
          // Con algo cargado alterna pausa; parado arranca la pista que esté
          // elegida en la lista (y si no hay ninguna elegida, la primera).
          if (MUSIC.isActive()) {
            MUSIC.togglePause();
          } else if (level == PLAYLIST && !tracks.empty()) {
            const int first = TRANSPORT_COUNT + 1;
            const int pick = selected >= first && rows[selected].kind == ROW_TRACK ? rows[selected].index : 0;
            MUSIC.playFolder(tracks, trackNames, folderName, folderPath, pick);
          }
          break;
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
    // Con UNA sola carpeta, subir un nivel deja una lista con un solo renglón y
    // parece que Atrás "borró" las canciones. Ahí Atrás sale del reproductor
    // directamente, que es lo que uno quiere decir.
    if (folders.size() <= 1) {
      activityManager.goHome();
      return;
    }
    level = FOLDERS;
    buildRows();
    selected = TRANSPORT_COUNT + 1;
    scroll = TRANSPORT_COUNT + 1;
    forceClean = true;
    requestUpdate();
    return;
  }
  activityManager.goHome();
}

void MusicActivity::loop() {
  // El contador y el analizador se repintan cada 2 s mientras suena.
  //
  // No puede ser cada segundo y no es un descuido: cada repintado es un refresco
  // parcial, y la regla del panel es un refresco COMPLETO cada 10-15 parciales.
  // A 1 Hz eso da un completo cada 12-24 s para siempre, que es justo lo que
  // deja fantasma y se come la batería. A 2 s el analizador se mueve y el
  // completo cae cada 48 s, que el panel aguanta mientras uno está mirando esta
  // pantalla. Estaba en 5 s y por eso parecía congelado.
  if (MUSIC.isSounding()) {
    const int sec = MUSIC.positionSeconds();
    if (sec / 2 != lastShownSecond / 2) requestUpdate();
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

// La duración se lee de la cabecera del archivo la primera vez que la fila
// aparece en pantalla y se guarda: pasar la lista no vuelve a tocar la tarjeta,
// y abrir una carpeta de 200 pistas no paga 200 lecturas de golpe.
int MusicActivity::trackDuration(const int index) {
  if (index < 0 || index >= static_cast<int>(trackSeconds.size())) return 0;
  if (trackSeconds[index] < 0) trackSeconds[index] = mp3DurationSeconds(tracks[index]);
  return trackSeconds[index];
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

// ── Dibujo: la skin ─────────────────────────────────────────────────────────

// Barra de título: negra con el nombre en blanco, como la de Winamp. Son 30 px
// de "chrome", no es el resalte de nada — el de la selección sigue siendo gris.
void MusicActivity::drawTitleBar(const int x, const int y, const int w, const int h) const {
  renderer.fillRoundedRect(x, y, w, h, 6, Color::Black);
  renderer.drawText(UI_10_FONT_ID, x + 12, y + (h - renderer.getTextHeight(UI_10_FONT_ID)) / 2, "CROSSPOINT AMP",
                    false, EpdFontFamily::BOLD);
  // Las rayitas de la derecha del título original.
  for (int i = 0; i < 7; ++i) renderer.fillRect(x + w - 16 - i * 6, y + 8, 2, h - 16, false);
}

// El analizador: el pico real de cada bloque decodificado, del más viejo al más
// nuevo. Sin audio quedan todas en el piso, que es exactamente lo que pasa.
// La altura de una barra a partir del pico crudo (0..127). En DECIBELES, como
// cualquier vúmetro: la música normal anda por los -18 dBFS y con escala lineal
// eso daba una barra de tres píxeles sobre cuarenta y cuatro — pegada al piso,
// que es justo lo que se veía. La escala va de -42 dBFS (piso) a 0 (lleno).
static int barHeightFromPeak(const int peak, const int h) {
  if (peak <= 0) return 2;
  constexpr float FLOOR_DB = -42.0f;
  const float db = 20.0f * log10f(static_cast<float>(peak) / 127.0f);
  if (db <= FLOOR_DB) return 2;
  const float frac = (db - FLOOR_DB) / -FLOOR_DB;
  const int bh = static_cast<int>(frac * static_cast<float>(h) + 0.5f);
  return bh < 2 ? 2 : (bh > h ? h : bh);
}

void MusicActivity::drawAnalyzer(const int x, const int y, const int w, const int h) const {
  const int bars = MusicPlayer::LEVELS;
  const int bw = std::max(2, (w - (bars - 1) * 2) / bars);
  for (int i = 0; i < bars; ++i) {
    const int bx = x + i * (bw + 2);
    const int level = MUSIC.isSounding() ? MUSIC.level(i) : 0;
    const int bh = barHeightFromPeak(level, h);
    renderer.fillRect(bx, y + h - bh, bw, bh, true);
    // Cabecita clara arriba de cada barra, como el "peak" del analizador.
    if (bh > 4) renderer.fillRect(bx, y + h - bh, bw, 2, false);
  }
  renderer.fillRect(x, y + h, w, 1, true);  // el piso
}

// El visor: contador de segmentos, estado, analizador, título, artista y la
// línea técnica. Todo lo que Winamp mete en su pantallita verde.
void MusicActivity::drawDisplay(const int x, const int y, const int w, const int h) const {
  renderer.fillRoundedRect(x, y, w, h, 4, Color::White);
  inset(renderer, x, y, w, h);

  const int pad = 12;
  const int segH = 44;
  const int seg = sevenseg::clock(renderer, MUSIC.positionSeconds(), x + pad, y + pad, 26, segH, 6, 5);

  // Analizador a la derecha del contador.
  const int anaX = x + pad + seg + 16;
  const int anaW = x + w - pad - anaX;
  if (anaW > 60) drawAnalyzer(anaX, y + pad, anaW, segH);

  // Título, artista y la línea técnica, abajo del visor.
  //
  // Se apilan DE ABAJO HACIA ARRIBA, midiendo cada fuente. Antes iban con saltos
  // fijos (+26, +50) desde arriba, y la línea de la calidad caía justo sobre el
  // marco del visor: se veía cortada al medio. Con las alturas reales de cada
  // fuente no puede pasar, cambie la tipografía que cambie.
  const int tw = w - 2 * pad;
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int subH = renderer.getLineHeight(UI_10_FONT_ID);
  const int techH = renderer.getLineHeight(SMALL_FONT_ID);
  const bool hasTech = MUSIC.isActive();
  const int techY = y + h - pad - techH;
  const int subY = (hasTech ? techY : y + h - pad) - 4 - subH;
  const int titleY = subY - 4 - titleH;

  // Estado y número de pista, entre el contador y el título. Iban a `y + pad + 50`,
  // un salto fijo que no sabía nada del título de abajo: con una tipografía más
  // alta (o un visor más bajo) "SONANDO 1/1" se montaba encima del nombre de la
  // pista. Ahora cuelga del contador y, si no entra, se corre hacia arriba.
  char sub[32] = "";
  if (MUSIC.isActive()) snprintf(sub, sizeof(sub), "%d/%d", MUSIC.index() + 1, MUSIC.count());
  const char* state = !MUSIC.isActive() ? tr(STR_MUSIC_STOPPED)
                      : MUSIC.isPaused() ? tr(STR_MUSIC_PAUSED)
                                         : tr(STR_MUSIC_PLAYING);
  const int stateH = renderer.getLineHeight(SMALL_FONT_ID);
  const int stateY = std::min(y + pad + segH + 6, titleY - 2 - stateH);
  renderer.drawText(SMALL_FONT_ID, x + pad, stateY, state, true, EpdFontFamily::BOLD);
  if (sub[0]) {
    renderer.drawText(SMALL_FONT_ID, x + pad + seg - renderer.getTextWidth(SMALL_FONT_ID, sub), stateY, sub);
  }

  const std::string title = MUSIC.isActive() ? MUSIC.title() : std::string(tr(STR_MUSIC_NOTHING_PLAYING));
  renderer.drawText(UI_12_FONT_ID, x + pad, titleY,
                    renderer.truncatedText(UI_12_FONT_ID, title.c_str(), tw, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  const std::string sub2 = MUSIC.isActive() ? (MUSIC.artist().empty() ? MUSIC.folderName() : MUSIC.artist())
                                            : std::string(tr(STR_MUSIC_HELP_START));
  renderer.drawText(UI_10_FONT_ID, x + pad, subY,
                    renderer.truncatedText(UI_10_FONT_ID, sub2.c_str(), tw).c_str());
  if (hasTech) {
    char tech[48];
    snprintf(tech, sizeof(tech), "%d kbps  %d kHz  %s", MUSIC.bitrateKbps(), MUSIC.sampleRate() / 1000,
             MUSIC.channels() == 1 ? tr(STR_MUSIC_MONO) : tr(STR_MUSIC_STEREO));
    renderer.drawText(SMALL_FONT_ID, x + pad, techY,
                      renderer.truncatedText(SMALL_FONT_ID, tech, tw).c_str());
  }
}

// Barra de posición con el cursor, hundida como la de Winamp.
void MusicActivity::drawPosition(const int x, const int y, const int w, const int h) const {
  char clock[40] = "--:-- / --:--";
  if (MUSIC.isActive()) {
    char now[16];
    char total[16];
    formatTime(now, sizeof(now), MUSIC.positionSeconds());
    formatTime(total, sizeof(total), MUSIC.durationSeconds());
    snprintf(clock, sizeof(clock), "%s / %s", now, total);
  }
  const int clockW = renderer.getTextWidth(SMALL_FONT_ID, clock) + 10;
  const int barW = w - clockW;
  renderer.fillRoundedRect(x, y, barW, h, 3, Color::White);
  inset(renderer, x, y, barW, h);
  const int span = MUSIC.durationSeconds();
  const int usable = barW - 6;
  const int fill = span > 0 ? std::min(usable, usable * MUSIC.positionSeconds() / span) : 0;
  if (fill > 0) renderer.fillRectDither(x + 3, y + 3, fill, h - 6, Color::DarkGray);
  // El cursor: un bloque macizo, que es lo que se ve de lejos.
  const int thumb = std::min(usable - 1, std::max(0, fill - 5));
  renderer.fillRect(x + 3 + thumb, y + 2, 10, h - 4, true);
  renderer.drawText(SMALL_FONT_ID, x + barW + 10, y + (h - renderer.getTextHeight(SMALL_FONT_ID)) / 2, clock);
}

// Los iconos de la botonera, dibujados a mano: a este tamaño quedan más nítidos
// que cualquier fuente y no dependen de nada bajado del servidor.
void MusicActivity::drawTransportIcon(const int action, const int cx, const int cy, const bool inverted) const {
  const bool ink = !inverted;  // sobre cara negra el glifo va en blanco
  switch (action) {
    case ACT_PREV:
      renderer.fillRect(cx - 11, cy - 8, 3, 16, ink);
      triangle(renderer, cx - 6, cy - 8, 8, false, ink);
      break;
    case ACT_PLAYPAUSE:
      if (MUSIC.isSounding()) {  // pausa
        renderer.fillRect(cx - 7, cy - 8, 5, 16, ink);
        renderer.fillRect(cx + 3, cy - 8, 5, 16, ink);
      } else {
        triangle(renderer, cx - 5, cy - 8, 8, true, ink);
      }
      break;
    case ACT_NEXT:
      triangle(renderer, cx - 3, cy - 8, 8, true, ink);
      renderer.fillRect(cx + 8, cy - 8, 3, 16, ink);
      break;
    case ACT_STOP:
      renderer.fillRect(cx - 7, cy - 7, 15, 15, ink);
      break;
    case ACT_SHUFFLE:
      // Las dos flechas cruzadas de siempre: cada una entra por la izquierda,
      // cruza en diagonal y sale por la derecha con su punta. Que se crucen EN
      // EL MEDIO es lo que hace que se lea "mezclar"; encimadas parecian un
      // garabato.
      renderer.drawLine(cx - 12, cy - 7, cx - 4, cy - 7, 2, ink);
      renderer.drawLine(cx - 4, cy - 7, cx + 5, cy + 6, 2, ink);
      triangle(renderer, cx + 5, cy + 2, 5, true, ink);
      renderer.drawLine(cx - 12, cy + 6, cx - 4, cy + 6, 2, ink);
      renderer.drawLine(cx - 4, cy + 6, cx + 5, cy - 7, 2, ink);
      triangle(renderer, cx + 5, cy - 11, 5, true, ink);
      break;
    case ACT_REPEAT:
      // Lazo CERRADO con un hueco arriba a la derecha y la punta saliendo por
      // ahi: asi se lee "vuelve a empezar". Abierto parecia exportar un archivo.
      renderer.drawLine(cx - 11, cy - 8, cx + 3, cy - 8, 2, ink);
      renderer.drawLine(cx + 11, cy - 5, cx + 11, cy + 8, 2, ink);
      renderer.drawLine(cx + 11, cy + 8, cx - 11, cy + 8, 2, ink);
      renderer.drawLine(cx - 11, cy + 8, cx - 11, cy - 8, 2, ink);
      triangle(renderer, cx + 4, cy - 13, 5, true, ink);
      break;
    default:
      break;
  }
}

// La botonera, a lo Winamp de verdad. Lo que habia eran seis rectangulos
// redondeados iguales, separados y con un brillito arriba: eso es un boton de
// telefono, no una botonera. Ahora:
//   - CUADRADOS, sin redondeo, con marco negro de 2 px y cara blanca;
//   - el transporte va PEGADO (los bordes se comparten, como la botonera
//     original), y los dos interruptores aparte a la derecha;
//   - el foco es un marco interior, no un relleno gris (en tinta el gris es
//     trama y deja fantasma);
//   - shuffle y repeat encendidos se pintan con la cara NEGRA y el glifo en
//     blanco, que es como los marca Winamp, en vez de una barrita abajo.
void MusicActivity::drawTransport(const int x, const int y, const int w, const int h) const {
  constexpr int TRANSPORT = 4;  // prev, play/pausa, siguiente, stop
  constexpr int TOGGLES = TRANSPORT_COUNT - TRANSPORT;
  constexpr int SEP = 18;       // aire entre el transporte y los interruptores
  const int bw = (w - SEP + (TRANSPORT - 1) * 2) / TRANSPORT_COUNT;
  const int tw = bw + (w - SEP - (TRANSPORT * bw - (TRANSPORT - 1) * 2) - TOGGLES * bw) / TOGGLES;

  for (int i = 0; i < TRANSPORT_COUNT; ++i) {
    const int action = rows.empty() ? i : static_cast<int>(rows[i].action);
    const bool toggle = i >= TRANSPORT;
    const bool on = (action == ACT_SHUFFLE && MUSIC.shuffle) || (action == ACT_REPEAT && MUSIC.repeat);
    const int cw = toggle ? tw : bw;
    const int bx = toggle ? x + TRANSPORT * (bw - 2) + SEP + (i - TRANSPORT) * cw : x + i * (bw - 2);
    renderer.fillRect(bx, y, cw, h, on);       // cara
    renderer.drawRect(bx, y, cw, h, 2, true);  // marco
    if (selected == i && !volumeMode) {
      renderer.drawRect(bx + 4, y + 4, cw - 8, h - 8, 2, !on);  // marco interior = foco
    }
    drawTransportIcon(action, bx + cw / 2, y + h / 2, on);
  }
}

// Volumen: "VOL" + la corredera y el número. Con el foco lleva marco grueso; en
// modo volumen, además, la corredera se dibuja rellena para que se note que la
// palanca la está moviendo.
void MusicActivity::drawVolume(const int x, const int y, const int w, const int h) const {
  const bool focused = !rows.empty() && selected < static_cast<int>(rows.size()) &&
                       rows[selected].kind == ROW_ACTION && rows[selected].action == ACT_VOLUME;
  if (focused) renderer.drawRoundedRect(x - 4, y - 3, w + 8, h + 6, volumeMode ? 3 : 2, 6, true);
  renderer.drawText(SMALL_FONT_ID, x, y + (h - renderer.getTextHeight(SMALL_FONT_ID)) / 2, "VOL", true,
                    EpdFontFamily::BOLD);
  char value[16];
  snprintf(value, sizeof(value), "%d %%", MUSIC.volume());
  const int valueW = renderer.getTextWidth(UI_10_FONT_ID, value) + 10;
  const int barX = x + 44;
  const int barW = w - 44 - valueW;
  renderer.fillRoundedRect(barX, y + 4, barW, h - 8, 3, Color::White);
  inset(renderer, barX, y + 4, barW, h - 8);
  const int fill = (barW - 6) * MUSIC.volume() / 100;
  if (fill > 0) renderer.fillRectDither(barX + 3, y + 7, fill, h - 14, Color::DarkGray);
  renderer.fillRect(barX + 3 + std::max(0, fill - 5), y + 6, 10, h - 12, true);
  renderer.drawText(UI_10_FONT_ID, x + w - valueW + 10, y + (h - renderer.getTextHeight(UI_10_FONT_ID)) / 2, value);
}

// La lista de abajo, con la pinta del editor de listas de Winamp: encabezado,
// número de pista y la que suena marcada.
void MusicActivity::drawPlaylist(const int x, const int y, const int w, const int h) {
  const bool inFolders = level == FOLDERS;
  renderer.drawRect(x, y, w, h, 2, true);
  // Cabecera de la lista: sobre BLANCO y separada por una regla de 1 px. La
  // barra negra con el texto en blanco era el único bloque macizo de esta mitad
  // de la pantalla y dejaba fantasma en el parcial siguiente; la regla no.
  const std::string header = inFolders ? std::string(tr(STR_MUSIC_CHOOSE_FOLDER)) : folderName;
  const int headerY = y + 3;
  const int right = x + w - LIST_PAD;
  const int listCount = static_cast<int>(rows.size()) - (TRANSPORT_COUNT + 1);
  int headerRoom = w - 2 * LIST_PAD;
  if (listCount > 0 && selected > TRANSPORT_COUNT) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%d / %d", selected - TRANSPORT_COUNT, listCount);
    const int pw = renderer.getTextWidth(UI_10_FONT_ID, pager);
    renderer.drawText(UI_10_FONT_ID, right - pw, headerY, pager);
    headerRoom -= pw + LIST_META_GAP;
  }
  if (headerRoom > 0) {
    renderer.drawText(
        UI_10_FONT_ID, x + LIST_PAD, headerY,
        renderer.truncatedText(UI_10_FONT_ID, header.c_str(), headerRoom, EpdFontFamily::BOLD).c_str(), true,
        EpdFontFamily::BOLD);
  }
  renderer.fillRect(x + 2, y + LIST_HEADER_H, w - 4, 1, true);

  const int top = y + LIST_HEADER_H + 4;
  const int listH = h - (LIST_HEADER_H + 4) - LIST_TAIL;
  if ((inFolders && folders.empty()) || (!inFolders && tracks.empty())) {
    renderer.drawText(UI_10_FONT_ID, x + LIST_PAD, top + 8, tr(STR_MUSIC_EMPTY), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, x + LIST_PAD, top + 34,
                      renderer.truncatedText(SMALL_FONT_ID, tr(STR_MUSIC_EMPTY_HELP), w - 2 * LIST_PAD).c_str());
    return;
  }

  clampScroll(top);
  const int numW = renderer.getTextWidth(UI_10_FONT_ID, "00");
  const int visible = std::min(listH / ROW_H, static_cast<int>(rows.size()) - scroll);
  for (int i = 0; i < visible; ++i) {
    const int idx = scroll + i;
    const Row& row = rows[idx];
    const int ry = top + i * ROW_H;
    if (idx == selected) drawSelectionRow(renderer, x + 3, ry, w - 6, ROW_H - 4, 6);

    char num[8] = "";
    std::string label;
    if (row.kind == ROW_FOLDER) {
      const std::string& p = folders[row.index];
      label = p == resolveMusicRoot() ? std::string("/") : p.substr(p.find_last_of('/') + 1);
    } else {
      snprintf(num, sizeof(num), "%02d", row.index + 1);
      label = trackNames[row.index];
    }
    // El texto arranca a LIST_PAD del borde del panel, o sea por FUERA de las
    // franjas tramadas que el resalte pone en los costados de la fila elegida.
    int textX = x + LIST_PAD;
    if (num[0]) {
      renderer.drawText(UI_10_FONT_ID, textX, ry + 6, num, SELECTION_INK);
      textX += numW + 10;
    }
    const bool sounding =
        row.kind == ROW_TRACK && MUSIC.isActive() && MUSIC.folderPath() == folderPath && MUSIC.index() == row.index;
    if (sounding) {
      triangle(renderer, textX, ry + 8, 6);
      textX += 14;
    }
    // Cuánto dura, alineado a la derecha en su columna: es el dato que uno mira
    // para elegir, y pegado al título no se lee ("Chan Chan 4:17").
    int metaW = 0;
    if (row.kind == ROW_TRACK) {
      const int seconds = trackDuration(row.index);
      if (seconds > 0) {
        char time[16];
        formatTime(time, sizeof(time), seconds);
        metaW = renderer.getTextWidth(UI_10_FONT_ID, time);
        renderer.drawText(UI_10_FONT_ID, right - metaW, ry + 6, time, SELECTION_INK);
        metaW += LIST_META_GAP;
      }
    }
    const int labelW = right - metaW - textX;
    if (labelW <= 0) continue;
    const EpdFontFamily::Style style = sounding ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_10_FONT_ID, textX, ry + 6,
                      renderer.truncatedText(UI_10_FONT_ID, label.c_str(), labelW, style).c_str(), SELECTION_INK,
                      style);
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
  int y = metrics.topPadding + 4;

  drawTitleBar(x, y, w, TITLEBAR_H);
  y += TITLEBAR_H + GAP;
  drawDisplay(x, y, w, DISPLAY_H);
  y += DISPLAY_H + GAP;
  drawPosition(x, y, w, POS_H);
  y += POS_H + GAP;
  drawTransport(x, y, w, TRANSPORT_H);
  y += TRANSPORT_H + GAP;
  drawVolume(x + 4, y, w - 8, VOL_H);
  y += VOL_H + GAP + 4;

  const int listH = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - y;
  drawPlaylist(x, y, w, listH);

  // La barra de abajo dice SIEMPRE qué hace cada botón acá y ahora.
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
