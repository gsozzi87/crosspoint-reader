#pragma once

#include <Arduino.h>

#include <cctype>

// Qué rutas de la tarjeta NO puede tocar nadie por la red, en UN solo lugar.
//
// Por qué un header compartido y no una copia en cada servidor: hasta 1.5.90
// esta regla estaba escrita DOS veces —una en `CrossPointWebServer.cpp` y otra
// en `WebDAVHandler.cpp`— y las dos copias se fueron separando. En 1.5.85 se
// arregló el agujero de `/download` en la primera y la segunda quedó como
// estaba; en 1.5.90 la auditoría encontró que `/upload` y `/mkdir` nunca habían
// llamado a ninguna de las dos. Una regla de seguridad repetida es una regla
// que alguien va a arreglar por la mitad.
//
// QUÉ SE PROTEGE Y POR QUÉ IMPORTA: adentro de `/.crosspoint` viven
// `server.json` (el token del aparato), `wifi.json` (las claves de las redes,
// ofuscadas con XOR+base64, o sea reversibles con el código de este mismo repo)
// y `device.log` (los nombres de las redes y TODO lo que el dueño dictó por
// voz). Y no es una red de confianza: en la ws397 el servidor web se levanta
// sobre un punto de acceso ABIERTO mientras se carga la clave del WiFi desde el
// teléfono (`WifiSelectionActivity`, `softAP(..., nullptr, ...)`), así que
// alcanza con estar cerca.
namespace protectedpaths {

inline constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};

// El alias 8.3 que FAT le arma a cada nombre largo esquiva un filtro por
// nombre: `.crosspoint` tiene de nombre corto `CROSSP~1` —que no empieza con
// punto y no está en HIDDEN_ITEMS— y `server.json` queda `SERVER~1.JSO`. Desde
// acá no se puede resolver el alias sin abrir el directorio, así que se rechaza
// la FORMA: `NOMBRE~<n>` con el nombre de a lo sumo seis caracteres y, si hay
// extensión, de tres como mucho.
//
// No se pierde nada usable: la lista de archivos de la web muestra siempre el
// nombre largo, así que nadie llega a un archivo tecleando su alias. Un
// `libro~1.epub` de verdad NO cae acá — `.epub` tiene cuatro letras y el 8.3 no
// llega a cuatro.
inline bool isEightDotThreeAlias(const String& name) {
  const int tilde = name.indexOf('~');
  if (tilde <= 0 || tilde > 6) return false;
  int i = tilde + 1;
  if (i >= static_cast<int>(name.length()) || !isdigit(static_cast<unsigned char>(name.charAt(i)))) return false;
  while (i < static_cast<int>(name.length()) && isdigit(static_cast<unsigned char>(name.charAt(i)))) ++i;
  if (i == static_cast<int>(name.length())) return true;  // sin extensión: CROSSP~1
  if (name.charAt(i) != '.') return false;
  return name.length() - i <= 4;  // ".JSO" y no ".epub"
}

// Un solo nombre, sin barras.
inline bool isProtectedName(const String& name) {
  if (name.startsWith(".")) return true;
  if (isEightDotThreeAlias(name)) return true;
  for (const auto* item : HIDDEN_ITEMS) {
    // Sin distinguir mayúsculas: FAT no las distingue, así que `xtcache` abre
    // el mismo directorio que `XTCache`.
    if (name.equalsIgnoreCase(item)) return true;
  }
  return false;
}

// La ruta ENTERA, segmento por segmento.
//
// Mirar sólo el último nombre era un agujero de verdad, no un detalle:
// `/download?path=/.crosspoint/server.json` daba `server.json`, que no empieza
// con punto y no está en HIDDEN_ITEMS, así que el servidor entregaba el TOKEN
// del aparato en claro (arreglado en 1.5.85).
inline bool isProtectedPath(const String& path) {
  int start = 0;
  while (start < static_cast<int>(path.length())) {
    if (path.charAt(start) == '/') {
      ++start;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();
    if (isProtectedName(path.substring(start, end))) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace protectedpaths
