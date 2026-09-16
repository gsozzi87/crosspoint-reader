#include "UITheme.h"

#include <BoardConfig.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/diario/DiarioTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

UITheme::UITheme() { setTheme(wantedTheme()); }

// El resto de las placas sigue con el selector de siempre sobre `uiTheme`.
CrossPointSettings::UI_THEME UITheme::wantedTheme() {
  // La ws397 tiene UN tema y no se elige: DIARIO. Es una decisión del dueño del
  // producto, textual: "vamos a dejar UN solo tema, no vamos a elegir más entre
  // uno y otro, vamos a dejar DIARIO".
  //
  // Va fijo acá y no en el valor guardado a propósito, para que el aparato que
  // venía con otro tema puesto en la tarjeta también pase a Diario sin migrar
  // nada. La fila STR_UI_THEME está escondida en esta placa, así que este es el
  // único lugar que lo decide.
  //
  // OJO: en 1.5.89 esto se cambió a LYRA. No lo pidió nadie, dejó a Diario sin
  // puerta —serif afuera, esquinas redondeadas adentro— y de paso reescribió
  // los comentarios como si siempre hubiera sido Lyra. Si alguien lo vuelve a
  // tocar, que sea porque el dueño cambió de opinión y lo dijo.
  if (BoardConfig::isWS397()) return CrossPointSettings::UI_THEME::DIARIO;
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
}

void UITheme::reload() { setTheme(wantedTheme()); }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  static const BaseTheme classic;
  static const LyraTheme lyra;
  static const RoundedRaffTheme roundedRaff;
  static const DiarioTheme diario;
  static const Lyra3CoversTheme lyra3Covers;
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = &classic;
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = &lyra;
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = &roundedRaff;
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::DIARIO:
      LOG_DBG("UI", "Using Diario theme");
      currentTheme = &diario;
      currentMetrics = &DiarioMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = &lyra3Covers;
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
  }
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  if (!metricsValid || touch != metricsForTouch) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    metricsForTouch = touch;
    metricsValid = true;
  }
  return adjustedMetrics;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  return (sb.textLaneVisible(true) ? (metrics.statusBarVerticalMargin) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

// Centered text implementation that takes the safe area into account.
// El texto se recorta contra el ancho real antes de centrarlo: si es más ancho
// que el área, `(width - textW) / 2` da una x negativa y el renderer termina
// dibujando fuera de la pantalla (ráfagas de "[GFX] !! Outside range" en el log
// del aparato, un renglón por pixel). Este es el único lugar por donde pasan
// todos los que centran contra el área segura, así que se arregla acá.
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  if (text == nullptr || *text == '\0' || screen.width <= 0) return;
  int textWidth = renderer.getTextWidth(fontId, text, style);
  if (textWidth <= screen.width) {
    renderer.drawText(fontId, screen.x + (screen.width - textWidth) / 2, y, text, black, style);
    return;
  }
  const std::string fitted = renderer.truncatedText(fontId, text, screen.width, style);
  textWidth = renderer.getTextWidth(fontId, fitted.c_str(), style);
  renderer.drawText(fontId, screen.x + std::max(0, (screen.width - textWidth) / 2), y, fitted.c_str(), black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
