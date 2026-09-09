#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

class SpeechOut;

// Notas de voz: la toma del micrófono queda tal cual en la tarjeta, en el mismo
// ADPCM que ya usan el servidor (`src/voice/Adpcm.h`) y los avisos hablados —
// una cuarta parte de un WAV, y `SpeechOut` lo reproduce sin convertir nada.
//
// Son LOCALES a propósito: no se suben, no salen en `GET /api/hub` y no las
// borra una sincronización. Viven en /.crosspoint/voicenotes/<epoch>.adpcm; sin
// reloj en hora el nombre pasa a ser "n<NNN>.adpcm" y se numeran por orden de
// llegada, que es lo único que se puede saber sin fecha.
namespace voicenotes {
constexpr const char* DIR = "/.crosspoint/voicenotes";
constexpr const char* EXT = ".adpcm";

struct Note {
  std::string path;   // "/.crosspoint/voicenotes/1757440000.adpcm"
  time_t epoch = 0;   // 0 = se grabó sin reloj en hora
  int number = 0;     // numeración de las que no tienen fecha
  int seconds = 0;    // duración, sacada del tamaño del archivo
};

void ensureDir();
// Las más nuevas primero (las que tienen fecha antes que las numeradas).
std::vector<Note> list();
// Guarda la toma ya codificada. Devuelve la nota creada en `out`.
bool save(const uint8_t* data, size_t len, Note& out);
bool remove(const Note& note);
// Reproduce por el parlante. No pasa por SpeechOut::playFile a propósito: ese
// corta en 256 KB (los clips de Piper son cortos) y una nota de voz de un
// minuto son 480 KB.
bool play(const Note& note, SpeechOut& out);

// "0:34", "1:05"
std::string mmss(int seconds);
// "09/09 14:20" con el huso del aparato, o "" si la nota no tiene fecha.
std::string when(const Note& note);

// Cuántos segundos de grabación entran de verdad en la memoria libre. Una toma
// de 16 kHz mono cuesta 32 KB/s de PCM más 8 KB/s de la copia en ADPCM (las dos
// están vivas a la vez mientras se sube o se guarda), así que el tope pedido no
// siempre se puede reservar: esto devuelve lo que entra, y la pantalla lo muestra.
uint32_t maxRecordSeconds(uint32_t wanted);
}  // namespace voicenotes
