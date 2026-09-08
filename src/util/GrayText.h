#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

// Pipeline de 4 grises para nuestras pantallas de texto.
//
// El lector de EPUB pinta la página primero en blanco y negro y después manda
// dos pasadas de grises (LSB y MSB) más un displayGrayBuffer(); con eso el
// texto queda suavizado (antialias de 4 niveles) en vez de duro y pixelado.
// Acá está lo mismo, empaquetado para cualquier pantalla nuestra que muestre
// una página de texto para leer.
//
// Cuesta ~600 ms extra por pantalla, así que se usa SOLO al pintar una página
// de lectura, nunca en el movimiento del cursor de una lista.
//
// Uso:
//   1) se dibuja todo en blanco y negro (encabezado, cuerpo, botones);
//   2) se llama a GrayText::displayPage(renderer, partialCount, dibujarCuerpo),
//      donde `dibujarCuerpo` vuelve a dibujar SOLO el cuerpo del texto (el
//      encabezado y los botones se quedan con la base en blanco y negro, igual
//      que la barra de estado del lector).
namespace GrayText {

// Regla del panel: refresco limpio cada 10-15 parciales o fantasmea.
constexpr int PARTIALS_BEFORE_CLEAN = 12;

// Alto de cada franja de la pasada por franjas (el mismo que usa el lector).
constexpr int STRIP_ROWS = 80;

// ¿Hay que suavizar el texto? (respeta el ajuste de Texto → Suavizado).
bool enabled();

// Modo de refresco que toca ahora y adelanta el contador de parciales.
HalDisplay::RefreshMode nextRefreshMode(int& partialCount);

namespace detail {
// Base en blanco y negro de una página que después recibe los grises.
void displayBase(const GfxRenderer& renderer, HalDisplay::RefreshMode mode);
// Cierre común de las dos pasadas: manda los grises al panel y vuelve a
// blanco y negro (`tiled` = camino por franjas, sin copia del framebuffer).
void finish(GfxRenderer& renderer, bool tiled);
// Salida sin grises (no hubo memoria): deja el panel y el modo consistentes.
void abortGray(GfxRenderer& renderer);
void logOom(int bytes);
}  // namespace detail

// Pinta la página ya dibujada en el framebuffer y le agrega los grises.
// `drawContent` se vuelve a llamar varias veces (una por pasada y franja), así
// que tiene que ser un dibujo repetible y sin efectos secundarios.
template <typename RenderFn>
void displayPage(GfxRenderer& renderer, int& partialCount, RenderFn&& drawContent) {
  const HalDisplay::RefreshMode mode = nextRefreshMode(partialCount);

  if (!enabled()) {
    renderer.displayBuffer(mode);
    return;
  }

  detail::displayBase(renderer, mode);

  if (renderer.supportsStripGrayscale()) {
    // Camino por franjas (el que usa el lector en esta placa): las pasadas van
    // directo a la RAM del controlador, sin copia entera del framebuffer.
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t scratchBytes = static_cast<size_t>(gwBytes) * STRIP_ROWS;
    auto* scratch = static_cast<uint8_t*>(malloc(scratchBytes));
    if (!scratch) {
      detail::logOom(static_cast<int>(scratchBytes));
      detail::abortGray(renderer);
      return;
    }
    for (int plane = 0; plane < 2; ++plane) {
      const bool lsb = plane == 0;
      renderer.setRenderMode(lsb ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = std::min(STRIP_ROWS, gh - y);
        renderer.beginStripTarget(scratch, y, rows);
        renderer.clearScreen(0x00);
        drawContent();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(lsb, scratch, y, rows);
      }
    }
    free(scratch);
    detail::finish(renderer, true);
    return;
  }

  // Paneles sin franjas: se guarda el framebuffer en blanco y negro, se pintan
  // las dos pasadas encima y después se restaura.
  if (!renderer.storeBwBuffer()) {
    detail::logOom(0);
    detail::abortGray(renderer);
    return;
  }
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  drawContent();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  drawContent();
  renderer.copyGrayscaleMsbBuffers();

  detail::finish(renderer, false);
}

}  // namespace GrayText
