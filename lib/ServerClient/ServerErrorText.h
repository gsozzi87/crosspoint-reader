#pragma once

#include <cstddef>
#include <string>

// REV-048: el mensaje de error del servidor, listo para un renglón.
//
// El servidor manda `{ok:false, error:"…", code:"…"}` y ese `error` es lo único
// que distingue un modelo que ya no existe de un proveedor caído, una clave
// vencida o el STT roto. Sacarlo del JSON es trabajo de ArduinoJson; lo que va
// acá es lo que viene después, que es donde están los errores propios:
//
//   - un salto de línea parte el renglón del log y no aporta nada;
//   - cortar a N bytes puede caer EN EL MEDIO de un carácter UTF-8, y medio
//     carácter en el visor es un cuadrito (los mensajes del proveedor vienen
//     con acentos y comillas tipográficas).
//
// Va en un header propio y sin nada del aparato adentro para poder probarlo de
// escritorio: ./test/server_error_text/run.sh
namespace servererr {

// Recorta a `maxBytes` sin partir un carácter UTF-8 y cambia los saltos de
// línea y tabulaciones por espacios. Si recortó, agrega "...".
inline std::string tidy(const std::string& in, size_t maxBytes) {
  std::string out = in;
  if (out.size() > maxBytes) {
    size_t cut = maxBytes;
    // Los bytes de continuación de UTF-8 son 10xxxxxx: se retrocede hasta el
    // primer byte del carácter. Como mucho tres pasos (un carácter son 4 bytes).
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    out += "...";
  }
  for (char& ch : out) {
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  }
  return out;
}

}  // namespace servererr
