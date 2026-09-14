#include "CardLayout.h"

#include <HalStorage.h>
#include <Logging.h>

#include <BoardConfig.h>

namespace cardlayout {
namespace {

constexpr const char* TAG = "CARD";

// El orden es el que se ve en el modo memoria USB, y ése es el punto: primero
// lo que el usuario carga (libros, música, apps) y después lo que sostiene al
// aparato (tipografías, diccionarios).
constexpr const char* CARPETAS[] = {
    "/Books",         // libros; el navegador arranca en "/" pero la carpeta orienta
    "/Music",         // MusicPlayer prueba varias grafías, ésta es la canónica
    "/Apps",          // apps en Lua
    "/fonts",         // SdCardFontRegistry::FONTS_DIR_VISIBLE
    "/dictionaries",  // DictionaryRegistry, raíz visible
};

}  // namespace

void ensure() {
  // Sólo la ws397: en las otras placas la tarjeta es del usuario y crear
  // carpetas que no pidió sería ruido.
  if (!BoardConfig::isWS397()) return;

  int creadas = 0;
  for (const char* carpeta : CARPETAS) {
    if (Storage.exists(carpeta)) continue;
    if (Storage.mkdir(carpeta)) {
      LOG_INF(TAG, "Carpeta creada: %s", carpeta);
      ++creadas;
    } else {
      // No es fatal: sin la carpeta todo sigue funcionando como hasta ahora
      // (la función que la use no encuentra nada). Se loguea y se sigue.
      LOG_ERR(TAG, "No se pudo crear %s", carpeta);
    }
  }
  if (creadas > 0) LOG_INF(TAG, "%d carpeta(s) creadas en la tarjeta", creadas);
}

}  // namespace cardlayout
