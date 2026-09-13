#pragma once

#include <functional>
#include <string>

class GfxRenderer;

// Un BMP de la tarjeta a pantalla completa, con los cuatro grises de verdad.
//
// Vivía como método estático de `PhotosActivity`. Las fotos salieron del
// producto, pero esto no es "de las fotos": lo usan la página del adjunto de un
// viaje (un PDF rasterizado por el servidor) y el fondo de pantalla del sueño.
//
// OJO con el modo: una sola pasada en BW pinta de negro TODO lo que no sea
// blanco puro (`drawBitmap` en BW: negro si val < 3, nada si val == 3), así que
// una imagen difuminada a cuatro niveles sale como una mancha. Hay que correr
// el pipeline de gris del SDK: base en blanco y negro + plano LSB + plano MSB +
// `displayGrayBuffer`.
//
// `baseOverlay` se dibuja SOLO sobre la base (los hints de botones, por
// ejemplo), no sobre los planos de gris.
namespace fullscreenbmp {
bool draw(GfxRenderer& renderer, const std::string& path,
          const std::function<void()>& baseOverlay = nullptr);
}
