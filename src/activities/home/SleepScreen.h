#pragma once

#include <string>
#include <vector>

class GfxRenderer;

// ws397: lo ÚLTIMO que se pinta antes de que el sistema se muera.
//
// El panel es biestable, así que lo que quede acá se queda en el vidrio con el
// aparato apagado y sin gastar nada. Por eso esta pantalla tiene que decir
// cosas que sigan siendo ciertas con el firmware muerto:
//
//   - La hora va como SELLO ("SUSPENDIDO · 21:53"), no como reloj. Nadie la va
//     a actualizar: mostrarla grande sería mentir.
//   - Lo que va en dígitos grandes es la PRÓXIMA ALARMA, que es justamente el
//     dato que no cambia mientras duerme.
//   - Los titulares y el clima son de cuando se pintó, y el pie lo dice.
//
// Y tiene que decir CÓMO se vuelve, que no es lo mismo en los dos casos:
// suspendido despierta OK (GPIO5, el único botón que es RTC GPIO en el S3) y
// apagado enciende PWR mantenido 1 s (PressOn del AXP2101, lo único que
// funciona sin ESP).
//
// Reemplaza a la pantalla de sueño del SDK y al fondo de pantalla con una foto:
// antes se pagaban DOS pinturas de pantalla completa antes de dormir.
namespace sleepscreen {

enum class State { Suspended, PoweredOff };

// Los titulares salen de la caché de Noticias (`/.crosspoint/rss/feeds.json`).
// Hay que leerlos ANTES de `Storage.prepareForDeepSleep()`, que desmonta la
// tarjeta; `paint()` ya no toca la SD.
std::vector<std::string> readHeadlines(int max = 6);

// Pinta y refresca. Devuelve false si no pudo (y entonces el llamador deja lo
// que hubiera: nunca se cuelga el sueño por esto).
bool paint(GfxRenderer& renderer, State state, const std::vector<std::string>& headlines);

}  // namespace sleepscreen
