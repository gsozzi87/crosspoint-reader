#pragma once

#include <Logging.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

// Iconos de 24 px propios del dialogo. No hay "check", "x" ni "papelera" en
// los juegos de iconos generados (listIcons/hubWidgetIcons son Lucide sueltos
// y en la nube no hay SVGs para regenerarlos), asi que estos tres van a mano,
// en el mismo formato del SDK: 1 bpp, MSB primero, bit 0 = tinta.
namespace optionpopup {

inline constexpr uint8_t icon_dlg_check_24_bits[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xF7, 0xFF, 0xFF, 0xE3, 0xFF, 0xFF, 0xC7, 0xFF, 0xFF, 0x87,
    0xFF, 0xFF, 0x0F, 0xFF, 0xFE, 0x1F, 0xFF, 0xFC, 0x3F, 0xFF, 0xFC, 0x7F,
    0xF7, 0xF8, 0xFF, 0xE3, 0xF0, 0xFF, 0xF1, 0xE1, 0xFF, 0xF8, 0xC3, 0xFF,
    0xFC, 0x07, 0xFF, 0xFE, 0x0F, 0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0xBF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
inline constexpr freeink::Icon icon_dlg_check_24 = {24, 24, 12, icon_dlg_check_24_bits};

inline constexpr uint8_t icon_dlg_close_24_bits[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFB, 0xFF, 0xDF, 0xF1, 0xFF, 0x8F, 0xF8, 0xFF, 0x1F, 0xFC, 0x7E, 0x3F,
    0xFE, 0x3C, 0x7F, 0xFF, 0x18, 0xFF, 0xFF, 0x81, 0xFF, 0xFF, 0xC3, 0xFF,
    0xFF, 0xC3, 0xFF, 0xFF, 0x81, 0xFF, 0xFF, 0x18, 0xFF, 0xFE, 0x3C, 0x7F,
    0xFC, 0x7E, 0x3F, 0xF8, 0xFF, 0x1F, 0xF1, 0xFF, 0x8F, 0xFB, 0xFF, 0xDF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
inline constexpr freeink::Icon icon_dlg_close_24 = {24, 24, 12, icon_dlg_close_24_bits};

inline constexpr uint8_t icon_dlg_trash_24_bits[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0xFF,
    0xFF, 0xBD, 0xFF, 0xFF, 0xBD, 0xFF, 0xE0, 0x00, 0x07, 0xFB, 0xFF, 0xDF,
    0xFB, 0xFF, 0xDF, 0xFB, 0xFF, 0xDF, 0xFB, 0xDD, 0xDF, 0xFB, 0xDD, 0xDF,
    0xFB, 0xDD, 0xDF, 0xFB, 0xDD, 0xDF, 0xF9, 0xDD, 0x9F, 0xFD, 0xDD, 0xBF,
    0xFD, 0xDD, 0xBF, 0xFD, 0xDD, 0xBF, 0xFD, 0xDD, 0xBF, 0xFD, 0xFF, 0xBF,
    0xFD, 0xFF, 0xBF, 0xFC, 0x00, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
inline constexpr freeink::Icon icon_dlg_trash_24 = {24, 24, 12, icon_dlg_trash_24_bits};

}  // namespace optionpopup

// Modal option picker drawn over the current screen (no clear).
//
// 1.5.48 lo dibuja a mano en vez de delegar en fui::optionDialog: hacia falta
// jerarquia tipografica (titulo UI_14 negrita, opciones UI_12, renglon de
// ayuda UI_10), un icono de 24 px por opcion cuando existe, y sobre todo que
// el resalte de la opcion elegida sea el de components/Selection.h (pestaña
// negra + marco + franjas al costado, centro BLANCO) y no una pastilla
// tramada con las letras encima. La caja se separa del fondo con un marco de
// 2 px y una sombra de 4 px en gris claro: la sombra es la superficie que mas
// contraste cambia al abrir y cerrar, asi que nada de negro ni de trama
// oscura ahi. El renglon de ayuda existe porque el dialogo era mudo: no decia
// como salir sin tocar nada.
//
// Touch hit-testing is the SDK's InteractionBuffer: each
// render registers the option buttons (plus a chrome guard rect) on the render
// task, and handleInput routes touch snapshots against that table on the loop
// task, gated by the uiReady handshake (same pattern as UiListActivity).
// render() builds into InteractionBuffer's non-published generation
// (beginPublishCycle()) and publishes it only once every hit() call for the
// frame is done (publish()), so handleInput()'s routePublished()/
// publishedData() reads on the loop task always see a complete table, never
// one render is mid-rebuilding. uiReady closes when show() replaces the
// popup's data, then stays open across ordinary repaints after the first
// publication so a release cannot be dropped during a highlight repaint.
//
// Background under the popup: processRender() snapshots the WHOLE framebuffer
// (48 KB, PSRAM) the first time it paints after show() and restores it before
// every repaint while the popup is open, so whatever third parties paint into
// the shared framebuffer in the meantime (an alarm Activity pushed on top and
// popped, the power-hold banner) never shows through around the dialog. Hosts
// may call processRender() before or after painting their page; either way
// the page under the popup is what the panel showed when the popup opened.
// The snapshot is freed on the first render after the popup closes, or with
// the popup; closing costs the host exactly one refresh (its own repaint).
class OptionPopup {
 public:
  OptionPopup() = default;
  ~OptionPopup() { freeUnderlay(); }
  OptionPopup(const OptionPopup&) = delete;
  OptionPopup& operator=(const OptionPopup&) = delete;

  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    onSelectCallback = std::move(onSelect);
    beginShow(currentIndex);
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    onSelectCallback = std::move(onSelect);
    beginShow(currentIndex);
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    onSelectCallback = std::move(onSelect);
    beginShow(currentIndex);
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    // NADA DE BOTONES HASTA QUE EL DIALOGO ESTE EN PANTALLA.
    //
    // En tinta un repintado tarda ~600 ms. Cualquier tecla que llegue antes es
    // una tecla que el usuario apreto SIN HABER VISTO el dialogo — venia de la
    // pantalla anterior, o la apreto de nuevo porque todavia no habia pasado
    // nada. Eso es lo que cerraba el menu de modo del temporizador antes de que
    // se llegara a ver: se abria y se volvia al hub solo.
    if (millis() - shownAtMs < SHOW_GRACE_MS) return true;

    // Y ADEMAS: OK y Atras valen al SOLTAR, pero solo si el dialogo vio la
    // PULSACION. Una tecla que se apreto antes de que el dialogo existiera (la
    // que lo abrio, o una de la pantalla anterior) y se suelta despues de la
    // gracia no es una decision sobre este dialogo. Sin esto, mantener OK un
    // poco mas de medio segundo al abrir el temporizador elegia la primera
    // opcion sin que nadie la eligiera.
    if (input.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed = true;
    if (input.wasPressed(MappedInputManager::Button::Back)) backArmed = true;

    // Match the render cap: only the first MAX_OPTIONS rows exist on screen,
    // so button wrap-around must not select an invisible option.
    const int total = static_cast<int>(ownedStrings.size());
    const int count = total > MAX_OPTIONS ? MAX_OPTIONS : total;
    // Un dialogo sin opciones (una busqueda que no encontro nada) se cierra sin
    // llamar al callback: el resto de la funcion divide por count al girar la
    // seleccion.
    if (count <= 0) {
      if (input.wasReleased(MappedInputManager::Button::Confirm) ||
          input.wasReleased(MappedInputManager::Button::Back)) {
        active = false;
        requestUpdate();
      }
      return true;
    }
    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Interactions are registered on the render task; only route once the
      // first render after show() has populated the table (uiReady handshake).
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          // Tap released on an option: select it, fire, dismiss.
          selectedIndex = event.value;
          active = false;
          if (onSelectCallback) onSelectCallback(selectedIndex);
          requestUpdate();
          return true;
        }
        if (event && event.action == ACTION_CHROME) {
          // Taps on the dialog chrome (title, padding) keep the popup open.
          return true;
        }
        if (snap.touchReleased && snap.touchX >= 0) {
          // Tap released outside the dialog: dismiss without firing. Swipe-end
          // releases arrive with -1,-1 coords and fall through (no dismiss).
          active = false;
          requestUpdate();
          return true;
        }
        if (snap.touchPressed) {
          // Touch-down on an option moves the highlight (route() latched the
          // hit as the active interaction; read it back, no re-hit-testing).
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && selectedIndex != hit.value) {
              selectedIndex = hit.value;
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    if (input.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::NavNext)) {
      selectedIndex = (selectedIndex + 1) % count;
      requestUpdate();
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!confirmArmed) {
        LOG_INF("POPUP", "OK soltado sin pulsacion vista (%lu ms tras abrir): se ignora", millis() - shownAtMs);
        return true;
      }
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Back)) {
      if (!backArmed) {
        LOG_INF("POPUP", "Atras soltado sin pulsacion vista (%lu ms tras abrir): se ignora", millis() - shownAtMs);
        return true;
      }
      LOG_INF("POPUP", "cerrado con Atras a los %lu ms", millis() - shownAtMs);
      active = false;
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) {
      // Closed: the host repaints its page now; the snapshot is done with.
      freeUnderlay();
      return false;
    }
    uint8_t* fb = renderer.getFrameBuffer();
    const size_t size = renderer.getBufferSize();
    if (fb && size > 0) {
      if (!underlay_ || underlaySize_ != size) {
        // First paint after show(): what is in the framebuffer now is the page
        // under the popup. Keep it whole; a rectangle would not survive a
        // third party repainting the rest of the frame while we are open.
        freeUnderlay();
        underlay_ = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!underlay_) underlay_ = static_cast<uint8_t*>(malloc(size));
        if (underlay_) {
          underlaySize_ = size;
          memcpy(underlay_, fb, size);
        }
      } else {
        memcpy(fb, underlay_, size);
      }
    }
    // "Cerrar" y no "Atrás": en un dialogo el boton de atras no vuelve a
    // ningun lado, cierra. El renglon de ayuda de abajo lo dice con todas las
    // letras.
    const auto popupLabels = input.mapLabels(tr(STR_HINT_CLOSE), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    // A cursor move repaints the same page + dialog with one row moved: a
    // plain FAST, which the refresh coordinator counts (and skips if nothing
    // changed at all).
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    namespace fui = freeink::ui;

    // Per-render target: a GfxRendererTarget is a renderer reference plus
    // three font ids, so rebuilding it here is trivially cheap and always
    // tracks the live orientation and uiScale fonts; a target held across
    // show() would stale-bind both after a rotation or scale change.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    refreshSharedUiThemeTokens(target);
    // Frame stores a const DeviceContext&; keep it in a local that outlives
    // the frame (a deviceContext() temporary would dangle).
    const fui::DeviceContext device = target.deviceContext();
    // Routing happens on the loop task against the member buffer; the frame
    // itself never dispatches, so it gets an empty snapshot.
    const fui::InputSnapshot noInput{};

    // Builds into the generation handleInput()'s routePublished()/
    // publishedData() aren't currently reading, so the loop task never sees
    // this table mid-rebuild — see publish() below and
    // InteractionBuffer::beginPublishCycle().
    interactions.beginPublishCycle();
    fui::Frame<INTERACTION_CAPACITY> frame(target, device, noInput, interactions);

    const Layout lay = layout(renderer);

    // Chrome guard first, options after: route() scans newest-first, so the
    // option rows win inside the dialog and the guard absorbs the rest.
    frame.hit(fui::Rect{static_cast<int16_t>(lay.x), static_cast<int16_t>(lay.y), static_cast<int16_t>(lay.w),
                        static_cast<int16_t>(lay.h)},
              ACTION_CHROME, 0, fui::InputTouch);
    for (int i = 0; i < lay.count; ++i) {
      const int rowY = lay.firstRowY + i * (lay.rowH + ROW_RULE);
      frame.hit(fui::Rect{static_cast<int16_t>(lay.innerX), static_cast<int16_t>(rowY),
                          static_cast<int16_t>(lay.innerW), static_cast<int16_t>(lay.rowH)},
                ACTION_OPTION, static_cast<int16_t>(i), fui::InputTouch);
    }

    paint(renderer, lay);

    // Atomically make this generation the one handleInput() reads, now that
    // every hit() call for this frame is done.
    interactions.publish();
    uiReady = true;
  }

  bool isActive() const { return active; }

  // Close without firing the callback (the surface under the popup is going
  // away, e.g. its host screen closes from outside the popup's own input).
  void dismiss() {
    active = false;
    onSelectCallback = nullptr;
    // The snapshot is released by the render task (next processRender) or the
    // destructor, never here: dismiss() runs on the loop task and a render
    // may be reading the snapshot right now.
  }

 private:
  // Geometria del dialogo. Un solo margen lateral de 24 px y todo en la
  // grilla de 8; el paso de renglon de las opciones es de 40 px (34 se leia
  // apretado, es la misma correccion que el visor de respuestas).
  static constexpr int SIDE_MARGIN = 24;
  static constexpr int MAX_DIALOG_W = 440;
  static constexpr int PAD = 16;
  static constexpr int TITLE_GAP = 8;
  static constexpr int HELP_GAP = 12;
  static constexpr int ROW_H = 40;
  static constexpr int ROW_H_MIN = 28;
  static constexpr int ROW_RULE = 1;
  static constexpr int ICON_W = 24;
  static constexpr int ICON_GAP = 12;
  // Aire que hay que dejarle al resalte de Selection.h: pestaña de 5 px +
  // franja tramada de 16 px + 3 px de inset. Con menos, el icono o la letra
  // caerian sobre la trama.
  static constexpr int SEL_CLEAR = 24;
  static constexpr int SHADOW = 4;

  struct Layout {
    int x = 0, y = 0, w = 0, h = 0;
    int innerX = 0, innerW = 0;
    int titleY = 0, ruleY = 0;
    int firstRowY = 0, rowH = ROW_H;
    int count = 0;
    int iconX = 0, labelX = 0, labelW = 0;
    int helpY = 0;
    std::vector<std::string> helpLines;
  };

  Layout layout(const GfxRenderer& renderer) const {
    Layout lay;
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int total = static_cast<int>(ownedStrings.size());
    lay.count = total > MAX_OPTIONS ? MAX_OPTIONS : total;

    lay.w = std::min(screenW - SIDE_MARGIN * 2, MAX_DIALOG_W);
    lay.x = (screenW - lay.w) / 2;
    lay.innerX = lay.x + PAD;
    lay.innerW = lay.w - PAD * 2;

    const int titleH = renderer.getLineHeight(UI_14_FONT_ID);
    lay.helpLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_DIALOG_HELP_BACK), lay.innerW, 2);
    const int helpH = static_cast<int>(lay.helpLines.size()) * renderer.getLineHeight(UI_10_FONT_ID);

    bool anyIcon = false;
    for (int i = 0; i < lay.count && i < static_cast<int>(icons.size()); ++i) {
      if (icons[i] != nullptr) anyIcon = true;
    }
    lay.iconX = lay.innerX + SEL_CLEAR;
    // Las opciones sin icono arrancan donde arrancan las que si lo tienen: la
    // columna de texto es una sola.
    lay.labelX = anyIcon ? lay.iconX + ICON_W + ICON_GAP : lay.iconX;
    lay.labelW = std::max(24, lay.innerX + lay.innerW - SEL_CLEAR - lay.labelX);

    const int chrome = PAD + titleH + TITLE_GAP + 1 + TITLE_GAP + HELP_GAP + helpH + PAD;
    // El dialogo se centra SOBRE la barra de botones, no sobre la pantalla
    // entera: la barra dice como salir y taparla es justo lo contrario de lo
    // que vino a arreglar el renglon de ayuda.
    const int bandH = screenH - UITheme::getInstance().getMetrics().buttonHintsHeight;
    const int avail = bandH - SIDE_MARGIN * 2;
    lay.rowH = ROW_H;
    int listH = lay.count * lay.rowH + std::max(0, lay.count - 1) * ROW_RULE;
    while (lay.rowH > ROW_H_MIN && chrome + listH > avail) {
      lay.rowH -= 2;
      listH = lay.count * lay.rowH + std::max(0, lay.count - 1) * ROW_RULE;
    }
    lay.h = std::min(chrome + listH, avail);
    lay.y = (bandH - lay.h) / 2;
    lay.titleY = lay.y + PAD;
    lay.ruleY = lay.titleY + titleH + TITLE_GAP;
    lay.firstRowY = lay.ruleY + 1 + TITLE_GAP;
    lay.helpY = lay.firstRowY + listH + HELP_GAP;
    return lay;
  }

  void paint(const GfxRenderer& renderer, const Layout& lay) const {
    // Sombra: dos franjas de 4 px en gris claro (derecha y abajo), y nada mas.
    // Un area grande de negro (o de trama oscura) es el peor fantasma posible,
    // porque aparece y desaparece de golpe al abrir y cerrar el dialogo. Se
    // dibujan las franjas y no el rectangulo entero corrido: se ve igual y no
    // se traman 250.000 pixeles para taparlos enseguida con el blanco.
    renderer.fillRectDither(lay.x + lay.w, lay.y + SHADOW, SHADOW, lay.h, Color::LightGray);
    renderer.fillRectDither(lay.x + SHADOW, lay.y + lay.h, lay.w, SHADOW, Color::LightGray);
    renderer.fillRect(lay.x, lay.y, lay.w, lay.h, false);
    renderer.drawRect(lay.x, lay.y, lay.w, lay.h, 2, true);

    // Titulo en UI_14 negrita + regla de 1 px: la jerarquia la hace la
    // tipografia, la regla solo separa superficies (y no cuesta tinta).
    const std::string titleLine =
        renderer.truncatedText(UI_14_FONT_ID, title.c_str(), lay.innerW, EpdFontFamily::BOLD);
    renderer.drawText(UI_14_FONT_ID, lay.innerX, lay.titleY, titleLine.c_str(), true, EpdFontFamily::BOLD);
    renderer.fillRect(lay.innerX, lay.ruleY, lay.innerW, 1, true);

    const int textH = renderer.getTextHeight(UI_12_FONT_ID);
    for (int i = 0; i < lay.count; ++i) {
      const int rowY = lay.firstRowY + i * (lay.rowH + ROW_RULE);
      if (i == selectedIndex) {
        // El resalte compartido: pestaña + marco + franjas al costado, centro
        // blanco. NUNCA letras sobre trama.
        drawSelectionRow(renderer, lay.innerX, rowY, lay.innerW, lay.rowH, 6);
      } else if (i > 0 && i - 1 != selectedIndex) {
        renderer.fillRect(lay.innerX + 8, rowY - ROW_RULE, lay.innerW - 16, 1, true);
      }
      const freeink::Icon* icon = i < static_cast<int>(icons.size()) ? icons[i] : nullptr;
      if (icon != nullptr) {
        BaseTheme::drawIconBitmap(renderer, *icon, lay.iconX, rowY + (lay.rowH - static_cast<int>(icon->h)) / 2);
      }
      const std::string label = renderer.truncatedText(UI_12_FONT_ID, ownedStrings[i].c_str(), lay.labelW);
      renderer.drawText(UI_12_FONT_ID, lay.labelX, rowY + (lay.rowH - textH) / 2, label.c_str());
    }

    // El renglon que faltaba: hasta 1.5.47 el dialogo no decia como salir sin
    // tocar nada.
    const int helpH = renderer.getLineHeight(UI_10_FONT_ID);
    for (size_t i = 0; i < lay.helpLines.size(); ++i) {
      renderer.drawText(UI_10_FONT_ID, lay.innerX, lay.helpY + static_cast<int>(i) * helpH,
                        lay.helpLines[i].c_str());
    }
  }

  // Icono de la opcion, por el texto ya traducido: las tres formas de show()
  // terminan con strings, asi que el que llama no tiene que cambiar nada para
  // que "Sí", "Cancelar" o "Borrar" salgan con su dibujo. Lo que no esta en la
  // tabla va sin icono (y alineado con los que si tienen).
  static const freeink::Icon* dialogIconForLabel(const char* label) {
    if (label == nullptr || label[0] == '\0') return nullptr;
    struct Entry {
      StrId id;
      const freeink::Icon* icon;
    };
    const Entry table[] = {
        {StrId::STR_YES, &optionpopup::icon_dlg_check_24},
        {StrId::STR_CONFIRM, &optionpopup::icon_dlg_check_24},
        {StrId::STR_OK_BUTTON, &optionpopup::icon_dlg_check_24},
        {StrId::STR_NO, &optionpopup::icon_dlg_close_24},
        {StrId::STR_CANCEL, &optionpopup::icon_dlg_close_24},
        {StrId::STR_DELETE, &optionpopup::icon_dlg_trash_24},
        // "Borrar" del hub (agenda y notas) y el "Atrás" que hace de cancelar
        // en el dialogo de borrado de las notas: son los dos dialogos que mas
        // se usan de todo el aparato.
        {StrId::STR_AGENDA_DELETE, &optionpopup::icon_dlg_trash_24},
        {StrId::STR_BACK, &optionpopup::icon_dlg_close_24},
        {StrId::STR_HINT_CLOSE, &optionpopup::icon_dlg_close_24},
        {StrId::STR_CLEAR_BUTTON, &optionpopup::icon_dlg_trash_24},
        {StrId::STR_UPDATE, &icon_download_24},
    };
    for (const Entry& entry : table) {
      const char* text = I18N.get(entry.id);
      if (text != nullptr && strcmp(text, label) == 0) return entry.icon;
    }
    return nullptr;
  }

  // Arranque comun de las tres formas de show(): el indice que llega del que
  // llama se acota a lo que el dialogo realmente dibuja (MAX_OPTIONS filas),
  // porque con un indice fuera de rango no se resaltaba ninguna fila y OK
  // devolvia ese mismo indice invalido al callback.
  void beginShow(int currentIndex) {
    const int total = static_cast<int>(ownedStrings.size());
    const int count = total > MAX_OPTIONS ? MAX_OPTIONS : total;
    selectedIndex = count > 0 ? std::min(std::max(currentIndex, 0), count - 1) : 0;
    resolveIcons();
    uiReady = false;
    shownAtMs = millis();
    confirmArmed = false;
    backArmed = false;
    active = true;
  }

  static constexpr unsigned long SHOW_GRACE_MS = 500;  // lo que tarda el panel en mostrarlo
  unsigned long shownAtMs = 0;
  bool confirmArmed = false;  // el dialogo vio apretar OK (no solo soltarlo)
  bool backArmed = false;

  void resolveIcons() {
    icons.assign(ownedStrings.size(), nullptr);
    for (size_t i = 0; i < ownedStrings.size(); ++i) {
      icons[i] = dialogIconForLabel(ownedStrings[i].c_str());
    }
  }

  // Frees the background snapshot. Only called from the render task
  // (processRender) or when the popup itself dies.
  void freeUnderlay() const {
    uint8_t* p = underlay_;
    underlay_ = nullptr;
    underlaySize_ = 0;
    if (p) free(p);
  }

  // The dialog has no scrolling, so options past MAX_OPTIONS would render off
  // screen anyway; a fixed cap keeps the row loop bounded and the interaction
  // table small. +1 slot for the chrome guard rect.
  static constexpr int MAX_OPTIONS = 16;
  static constexpr size_t INTERACTION_CAPACITY = MAX_OPTIONS + 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;

  bool active = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  // Icono de cada opcion, o nullptr. Paralelo a ownedStrings; lo llena
  // resolveIcons() al abrir.
  std::vector<const freeink::Icon*> icons;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  // Written by the render task (frame registration), routed by the loop task;
  // uiReady closes the rebuild window exactly like UiListActivity::uiReady.
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
  // Whole-framebuffer snapshot of the page under the popup (see class doc).
  // Owned by the render task; mutable because processRender() is const.
  mutable uint8_t* underlay_ = nullptr;
  mutable size_t underlaySize_ = 0;
};
