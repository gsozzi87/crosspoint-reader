#include "FullScreenBmp.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace {
constexpr const char* TAG = "BMP";
}

// Pantalla completa, centrada, con los 4 grises de verdad. Ojo: una sola pasada
// en modo BW pinta de negro TODO lo que no sea blanco puro (drawBitmap en BW:
// negro si val < 3, nada si val == 3), así que una foto difuminada a 4 niveles
// salía como una mancha negra. Hay que correr el pipeline de gris del SDK:
// base en blanco y negro + plano LSB + plano MSB + displayGrayBuffer.
bool fullscreenbmp::draw(GfxRenderer& renderer, const std::string& path,
                         const std::function<void()>& baseOverlay) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "no se pudo abrir %s", path.c_str());
    return false;
  }
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR(TAG, "BMP inválido: %s", path.c_str());
    return false;
  }

  int x = 0, y = 0;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      y = std::round((pageHeight - pageWidth / ratio) / 2);
    } else {
      x = std::round((pageWidth - pageHeight * ratio) / 2);
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  if (!bitmap.hasGreyscale()) {
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    if (baseOverlay) baseOverlay();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  // Los tres planos se arman ANTES de tocar la pantalla. Leer y escalar el BMP
  // desde la SD tarda casi un segundo por pasada, y hacerlo entre el destello
  // de la base y el empujón de gris dejaba la versión en blanco y negro a la
  // vista todo ese rato (los "cuatro destellos" que se veían). Con los planos
  // guardados en PSRAM, la base y el gris salen pegados y se ve una sola
  // aparición. Si no hay PSRAM para los planos, se cae al orden clásico.
  const size_t bufferSize = renderer.getBufferSize();
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  uint8_t* lsb = static_cast<uint8_t*>(heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* msb = lsb ? static_cast<uint8_t*>(heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
  const bool staged = lsb && msb && frameBuffer;

  // Cada pasada dibuja lo mismo: la foto y lo que le va encima. El overlay tiene
  // que entrar también en los planos de gris, si no el empujón de gris lo borra
  // (dibuja TODA la pantalla) y la barra de botones se pierde.
  const auto drawPass = [&] {
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    if (baseOverlay) baseOverlay();
  };

  if (staged) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    drawPass();
    memcpy(lsb, frameBuffer, bufferSize);

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    drawPass();
    memcpy(msb, frameBuffer, bufferSize);

    bitmap.rewindToData();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // La base tiene que ser HALF: la LUT del empujón de gris está calibrada
  // contra el estado que deja esa forma de onda.
  renderer.clearScreen();
  drawPass();
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  if (staged) {
    memcpy(frameBuffer, lsb, bufferSize);
    renderer.copyGrayscaleLsbBuffers();
    memcpy(frameBuffer, msb, bufferSize);
    renderer.copyGrayscaleMsbBuffers();
  } else {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    drawPass();
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    drawPass();
    renderer.copyGrayscaleMsbBuffers();
  }

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  heap_caps_free(lsb);
  heap_caps_free(msb);
  return true;
}
