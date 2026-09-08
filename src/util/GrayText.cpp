#include "GrayText.h"

#include <Logging.h>

#include "CrossPointSettings.h"

namespace {
constexpr const char* TAG = "GRAYTEXT";
}

namespace GrayText {

bool enabled() { return SETTINGS.textAntiAliasing != 0; }

HalDisplay::RefreshMode nextRefreshMode(int& partialCount) {
  // Regla del panel: refresco limpio cada 10-15 parciales o fantasmea.
  if (++partialCount >= PARTIALS_BEFORE_CLEAN) {
    partialCount = 0;
    return HalDisplay::HALF_REFRESH;
  }
  return HalDisplay::FAST_REFRESH;
}

namespace detail {

void displayBase(const GfxRenderer& renderer, const HalDisplay::RefreshMode mode) {
  // Los paneles que combinan la base (Paper Mono) difieren la activación para
  // que base y grises salgan en una sola forma de onda; los demás la muestran
  // como siempre.
  if (renderer.combinesGrayscaleBase()) {
    renderer.displayGrayscaleBase(mode);
  } else {
    renderer.displayBuffer(mode);
  }
}

void finish(GfxRenderer& renderer, const bool tiled) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  if (tiled) {
    // Por franjas el framebuffer nunca se tocó: solo hay que rehacer la
    // referencia diferencial del controlador con lo que ya tiene adentro.
    renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    renderer.restoreBwBuffer();
  }
}

void abortGray(GfxRenderer& renderer) {
  renderer.setRenderMode(GfxRenderer::BW);
  // Si el panel difirió la base, hay que comprometerla igual o la página no
  // llega a la pantalla.
  if (renderer.combinesGrayscaleBase()) {
    renderer.cleanupGrayscaleWithFrameBuffer();
  }
}

void logOom(const int bytes) { LOG_ERR(TAG, "sin memoria para los grises (%d bytes); la pagina sale sin suavizar", bytes); }

}  // namespace detail

}  // namespace GrayText
