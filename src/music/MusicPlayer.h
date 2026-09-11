#pragma once

#include <AudioManager.h>

#include <string>
#include <vector>

#include "music/Mp3Source.h"

// El reproductor de música del aparato, uno solo y VIVO FUERA DE LA PANTALLA.
//
// Antes todo esto vivía adentro de MusicActivity, así que salir del
// reproductor cortaba la canción y el hub no tenía forma de saber qué estaba
// sonando. Ahora el estado vive acá: se entra y se sale de la pantalla y la
// música sigue, el hub muestra la pista en la barra, y los pitidos de interfaz
// saben que hay música y se callan ("cuando se reproduzca la música los demás
// sonidos no deben oirse, sólo la música").
//
// `pump()` se llama desde el loop de main.cpp: es lo que encadena la pista
// siguiente cuando la anterior termina, esté abierta la pantalla o no.
class MusicPlayer {
 public:
  static MusicPlayer& instance();

  // Carga una carpeta entera y arranca por `index`. Se queda con una copia de
  // la lista: la pantalla puede irse a mirar otra carpeta sin tocar lo que suena.
  bool playFolder(const std::vector<std::string>& paths, const std::vector<std::string>& names,
                  const std::string& folderName, const std::string& folderPath, int index);
  bool play(int index);
  void stop();
  void togglePause();
  void next(bool fromEnd);
  void previous();
  void setVolume(int value);
  void pump();  // desde el loop principal

  bool isActive() const { return active_; }               // hay una pista cargada
  bool isSounding() const { return active_ && !paused_; }  // suena de verdad
  bool isPaused() const { return paused_; }
  bool shuffle = false;
  bool repeat = false;

  // El volumen vive en HubStore (es UNO SOLO para música, voz y avisos), así
  // que se lee de ahí: cambiarlo desde Ajustes o desde la web se ve acá sin
  // tener que sincronizar dos copias.
  int volume() const;
  int index() const { return index_; }
  int count() const { return static_cast<int>(names_.size()); }
  int positionSeconds() const { return active_ ? source_.positionSeconds() : 0; }
  int durationSeconds() const { return active_ ? source_.durationSeconds() : 0; }
  const std::string& folderName() const { return folderName_; }
  const std::string& folderPath() const { return folderPath_; }
  const std::vector<std::string>& trackNames() const { return names_; }
  // Datos técnicos para el panel del reproductor (la línea "192 kbps 44 kHz").
  int bitrateKbps() const { return active_ ? source_.bitrateKbps() : 0; }
  int sampleRate() const { return active_ ? source_.sampleRate() : 0; }
  int channels() const { return active_ ? source_.channels() : 0; }
  // Barra i del analizador (0..15), de la más vieja a la más nueva.
  uint8_t level(const int i) const { return active_ ? source_.level(i) : 0; }
  static constexpr int LEVELS = Mp3Source::LEVELS;
  // Título de la pista: la etiqueta ID3 si la tiene, si no el nombre del archivo.
  std::string title() const;
  const std::string& artist() const { return artist_; }
  // Una línea corta para la barra del hub: "Título — Artista".
  std::string nowPlayingLine() const;

 private:
  MusicPlayer() = default;

  AudioManager audio_;
  Mp3Source source_;
  std::vector<std::string> paths_;
  std::vector<std::string> names_;
  std::string folderName_;
  std::string folderPath_;
  std::string artist_;
  int index_ = -1;
  bool active_ = false;
  bool paused_ = false;
  bool begun_ = false;
};

#define MUSIC MusicPlayer::instance()
