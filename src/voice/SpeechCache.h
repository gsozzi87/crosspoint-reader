#pragma once

#include <string>

// Ruta del clip hablado (Piper) cacheado en la SD para un texto.
//
// El nombre lleva un hash de (voz + idioma + texto), así que un cambio de voz
// en el servidor, un cambio de idioma o un título distinto dan otro archivo: el
// clip viejo no se puede reusar por accidente. Antes el nombre era solo
// "timer-es.bin" o "r<id>.bin" y el aviso seguía sonando con la voz vieja para
// siempre (o con el título viejo del recordatorio).
namespace speechcache {
constexpr const char* DIR = "/.crosspoint/tts";

std::string clipPath(const std::string& text);
}  // namespace speechcache
