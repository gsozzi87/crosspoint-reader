#pragma once

#include <string>

// Rolling log on the SD (/.crosspoint/device.log, two files rotated at 64 KB):
// every LOG_* line also lands here, with the RTC time when it is set, so a
// problem that happened while the device was away from the cable can still be
// read afterwards. Uploaded to the server on each hub sync and shown at
// /board, and downloadable from the device's own web UI.
//
// Lo que entra está deduplicado: las líneas iguales seguidas (o iguales salvo
// los números, que es lo mismo para el que lee) se guardan una sola vez con un
// "(x123)" al final. Sin eso un solo dibujo fuera de rango llenaba los 64 KB
// con la misma línea y tapaba lo único que servía para depurar.
namespace devlog {
void begin();                  // opens the file, writes the session header
void write(const char* line);  // called by Logging for every line
void flush();                  // called before sleeping / rebooting
// Anota un evento propio (arranque de una pantalla, resultado de una llamada al
// servidor, un error con contexto). Sale con el prefijo "* " y NO se deduplica:
// es lo que se quiere ver entero aunque se repita.
void event(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Cierra el log: después de esto no se escribe más (la SD se desmonta antes de
// dormir y escribir sobre un filesystem desmontado se pierde en silencio).
void close();
// Whole log (current + previous), capped at maxBytes from the end.
std::string tail(size_t maxBytes = 32 * 1024);
size_t size();
}  // namespace devlog
