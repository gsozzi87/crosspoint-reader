#include "MusicPlayer.h"

#include <Logging.h>
#include <esp_random.h>

#include <algorithm>

#include "HubStore.h"

namespace {
constexpr const char* TAG = "MUSIC";
}

MusicPlayer& MusicPlayer::instance() {
  static MusicPlayer player;
  return player;
}

bool MusicPlayer::playFolder(const std::vector<std::string>& paths, const std::vector<std::string>& names,
                             const std::string& folderName, const std::string& folderPath, const int index) {
  paths_ = paths;
  names_ = names;
  folderName_ = folderName;
  folderPath_ = folderPath;
  return play(index);
}

bool MusicPlayer::play(const int index) {
  if (index < 0 || index >= static_cast<int>(paths_.size())) return false;
  audio_.stop();
  active_ = false;
  paused_ = false;
  artist_.clear();

  if (!source_.open(paths_[index])) {
    LOG_ERR(TAG, "no se pudo abrir %s", paths_[index].c_str());
    index_ = -1;
    return false;
  }
  // Todo camino de error cierra la fuente: si no quedan colgados el archivo y
  // los buffers del decodificador.
  if (!begun_) {
    if (!audio_.begin()) {
      LOG_ERR(TAG, "el codec no arranco");
      source_.close();
      index_ = -1;
      return false;
    }
    begun_ = true;
  }
  audio_.setVolume(volume());
  if (!audio_.play(source_.wavSource(), false)) {
    LOG_ERR(TAG, "el reproductor no acepto la pista");
    source_.close();
    index_ = -1;
    return false;
  }
  index_ = index;
  artist_ = source_.artist();
  active_ = true;
  paused_ = false;
  LOG_INF(TAG, "suena %d/%u %s", index + 1, static_cast<unsigned>(paths_.size()), title().c_str());
  return true;
}

void MusicPlayer::stop() {
  if (active_) audio_.stop();
  source_.close();
  active_ = false;
  paused_ = false;
  index_ = -1;
  if (begun_) {
    // Soltar el I2S: si no, el micrófono no puede abrirse después de escuchar
    // música ("Falló la captura del micrófono").
    audio_.end();
    begun_ = false;
  }
}

// El SDK no tiene pausa: parar y volver a reproducir arrancaría la pista de
// cero, así que la pausa baja el volumen a cero y deja el stream corriendo.
void MusicPlayer::togglePause() {
  if (!active_) return;
  paused_ = !paused_;
  // Pausar era SOLO bajar el volumen a 0: el MP3 se seguia decodificando y el
  // I2S se seguia escribiendo a velocidad de hardware, asi que la pausa gastaba
  // lo mismo que sonar y la pista se terminaba sola estando "en pausa" (y
  // pump() encadenaba la siguiente). setPaused() frena de verdad la tarea de
  // audio; el volumen 0 queda igual, para que no se escape ni un pedacito de
  // muestra entre la bandera y el silencio del DMA.
  audio_.setVolume(paused_ ? 0 : volume());
  audio_.setPaused(paused_);
}

void MusicPlayer::next(const bool fromEnd) {
  if (paths_.empty()) return;
  int idx;
  if (shuffle && paths_.size() > 1) {
    do {
      idx = static_cast<int>(esp_random() % paths_.size());
    } while (idx == index_);
  } else {
    idx = index_ + 1;
    if (idx >= static_cast<int>(paths_.size())) {
      if (!repeat && fromEnd) {
        stop();
        return;
      }
      idx = 0;
    }
  }
  play(idx);
}

void MusicPlayer::previous() {
  if (paths_.empty()) return;
  int idx = index_ - 1;
  if (idx < 0) idx = static_cast<int>(paths_.size()) - 1;
  play(idx);
}

// Un solo volumen para todo el aparato (música, voz de Piper y pitidos), así
// que se guarda apenas cambia y también se puede tocar desde Ajustes.
void MusicPlayer::setVolume(const int value) {
  const int clamped = std::max(0, std::min(100, value));
  if (HUB_STORE.musicVolume != clamped) {
    HUB_STORE.musicVolume = clamped;
    HUB_STORE.saveToFile();
  }
  if (active_) audio_.setVolume(paused_ ? 0 : clamped);
}

int MusicPlayer::volume() const {
  const int v = HUB_STORE.musicVolume;
  return v < 0 ? 0 : v > 100 ? 100 : v;
}

// Encadena la pista siguiente cuando la de ahora se termina. Se llama desde el
// loop de main.cpp, así que sigue funcionando con la pantalla del reproductor
// cerrada (es lo que permite que el hub muestre lo que está sonando).
void MusicPlayer::pump() {
  if (!active_ || paused_) return;
  if (!audio_.isPlaying()) next(true);
}

std::string MusicPlayer::title() const {
  if (!active_) return "";
  if (!source_.title().empty()) return source_.title();
  if (index_ >= 0 && index_ < static_cast<int>(names_.size())) return names_[index_];
  return "";
}

std::string MusicPlayer::nowPlayingLine() const {
  if (!active_) return "";
  std::string line = title();
  if (!artist_.empty()) line += " - " + artist_;
  return line;
}
