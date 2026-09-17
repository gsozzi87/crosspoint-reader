#include "TaskStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "TaskConfig.h"
#include "activities/ListStyle.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BatteryLog.h"
#include "util/IdleSleep.h"

namespace {
// Las tareas que se muestran, en orden de "qué tan cerca está del límite".
constexpr tasks::Id SHOWN[] = {tasks::Id::Loop, tasks::Id::Render, tasks::Id::UiSound, tasks::Id::AudioPlay};
constexpr size_t SHOWN_COUNT = sizeof(SHOWN) / sizeof(SHOWN[0]);

// KB con un decimal hasta 100 KB y entero de ahí para arriba: "7,8 KB" dice
// algo, "1234,6 KB" es ruido.
void formatBytes(char* out, const size_t outSize, const uint32_t bytes) {
  const float kb = bytes / 1024.0f;
  if (kb < 100.0f) {
    snprintf(out, outSize, "%.1f KB", kb);
  } else {
    snprintf(out, outSize, "%u KB", static_cast<unsigned>(bytes / 1024));
  }
}

void formatDuration(char* out, const size_t outSize, const unsigned long ms) {
  const unsigned long s = ms / 1000;
  if (s < 60) {
    snprintf(out, outSize, "%lu s", s);
  } else if (s < 3600) {
    snprintf(out, outSize, "%lu min", s / 60);
  } else {
    snprintf(out, outSize, "%lu h %lu min", s / 3600, (s % 3600) / 60);
  }
}

uint32_t absDiff(const uint32_t a, const uint32_t b) { return a > b ? a - b : b - a; }

// El cabezal de esta pantalla se dibuja a mano (título UI_14 + regla), así que
// su pie NO es `listui::contentTop()`, que sale del tema. Las dos cuentas que
// dependen de él —el recorte al pintar y el tope del desplazamiento— tienen
// que salir de la misma función o la última fila queda inalcanzable.
int tituloBaseY(const GfxRenderer& r) { return listui::SIDE + r.getTextHeight(UI_14_FONT_ID) + listui::GAP; }
int viewTopY(const GfxRenderer& r) { return tituloBaseY(r) + 1 + listui::GAP; }
}  // namespace

TaskStatsActivity::Snapshot TaskStatsActivity::take() {
  Snapshot s;
  s.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.internalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s.internalBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.psramBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  for (size_t i = 0; i < SHOWN_COUNT && i < 8; ++i) s.stackFree[i] = tasks::usage(SHOWN[i]).freeBytes;
  s.restCycles = IDLE_SLEEP.cycles();
  return s;
}

bool TaskStatsActivity::worthRepainting(const Snapshot& a, const Snapshot& b) {
  if (absDiff(a.internalFree, b.internalFree) >= REPAINT_BYTES) return true;
  if (a.internalMin != b.internalMin) return true;
  if (absDiff(a.internalBlock, b.internalBlock) >= REPAINT_BYTES) return true;
  if (absDiff(a.psramFree, b.psramFree) >= REPAINT_BYTES) return true;
  if (a.restCycles != b.restCycles) return true;
  for (size_t i = 0; i < SHOWN_COUNT && i < 8; ++i) {
    if (a.stackFree[i] != b.stackFree[i]) return true;
  }
  return false;
}

void TaskStatsActivity::onEnter() {
  Activity::onEnter();
  painted = Snapshot{};
  lastPaint = 0;
  requestUpdate();
}

void TaskStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // La palanca corre la pantalla. Se hace antes del repintado automático para
  // que un movimiento se vea al toque y no después de los dos segundos.
  const int maxScroll = std::max(0, contentH - (listui::contentBottom(renderer) - viewTopY(renderer)));
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && scroll < maxScroll) {
    scroll = std::min(maxScroll, scroll + SCROLL_STEP);
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) && scroll > 0) {
    scroll = std::max(0, scroll - SCROLL_STEP);
    requestUpdate();
    return;
  }

  if (millis() - lastPaint < REFRESH_MS) return;
  // La regla del panel manda: se repinta sólo si algún número se movió de
  // verdad. Un contador que cambia por 16 bytes deja fantasma y no dice nada.
  if (worthRepainting(painted, take())) requestUpdate();
  lastPaint = millis();
}

void TaskStatsActivity::render(RenderLock&&) {
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  // Todo lo de abajo se dibuja en coordenadas "del documento" y se baja por
  // `scroll` al pintar. `emit` es el que decide si un bloque cae dentro del
  // área útil: lo que queda arriba del encabezado o debajo de la barra de
  // botones no se dibuja (si se dibujara igual, GfxRenderer lo contaría como
  // píxeles fuera de pantalla y además taparía los botones, que es justo el
  // bug que esto arregla).
  // El tope del área útil es el pie REAL del cabezal de esta pantalla (que se
  // dibuja a mano, no con el del tema), no `listui::contentTop()`: si los dos
  // no coinciden, la primera fila visible se dibuja por encima del título.
  const int tituloBase = tituloBaseY(renderer);
  const int viewTop = viewTopY(renderer);
  const int viewBottom = listui::contentBottom(renderer);
  const auto visible = [&](const int docY, const int height) {
    const int screenY = docY - scroll;
    return screenY + height > viewTop && screenY < viewBottom;
  };
  // Los dos envoltorios: toman la y del documento, la bajan por `scroll` y no
  // dibujan lo que no entra. Devuelven la y siguiente, así el cuerpo de abajo
  // se sigue leyendo igual que antes.
  const auto header = [&](const int docY, const char* title, const char* datum) {
    if (visible(docY, listui::SECTION_H)) listui::sectionHeader(renderer, x, docY - scroll, w, title, datum);
    return docY + listui::SECTION_H;
  };
  const auto rowAt = [&](const int docY, const int h, const listui::RowSpec& spec) {
    if (visible(docY, h)) listui::row(renderer, x, docY - scroll, w, h, spec);
    return docY + h;
  };
  const Snapshot now = take();
  char left[64];
  char right[64];
  char detail[96];

  renderer.clearScreen();

  // El cabezal se dibuja DESPUÉS del contenido (abajo del todo). `visible()`
  // deja pasar la fila que cruza el borde —tiene que hacerlo, o la lista
  // parpadearía de a saltos—, así que esa fila sobresale por arriba; pintar el
  // cabezal encima al final es lo que la corta limpio.
  int y = viewTop;

  // --- Heap ---------------------------------------------------------------
  // La memoria interna es la que se acaba primero (el TLS y los buffers de I2S
  // no pueden vivir en PSRAM), así que va primera y con el mínimo histórico al
  // lado: el promedio no sirve, lo que importa es el peor momento.
  formatBytes(left, sizeof(left), now.internalFree);
  y = header(y, tr(STR_MEMORY_INTERNAL), left);
  formatBytes(left, sizeof(left), now.internalMin);
  formatBytes(right, sizeof(right), now.internalBlock);
  snprintf(detail, sizeof(detail), "%s: %s  ·  %s: %s", tr(STR_MEMORY_MIN), left, tr(STR_MEMORY_LARGEST), right);
  y = rowAt(y, listui::ROW1_H, {.title = detail, .rule = true});
  y += listui::GAP;  // rowAt ya sumó el alto de la fila

  formatBytes(left, sizeof(left), now.psramFree);
  y = header(y, tr(STR_MEMORY_PSRAM), left);
  formatBytes(right, sizeof(right), now.psramBlock);
  snprintf(detail, sizeof(detail), "%s: %s", tr(STR_MEMORY_LARGEST), right);
  y = rowAt(y, listui::ROW1_H, {.title = detail, .rule = true});
  y += listui::GAP;  // rowAt ya sumó el alto de la fila

  // --- Tareas -------------------------------------------------------------
  y = header(y, tr(STR_MEMORY_TASKS), nullptr);
  for (size_t i = 0; i < SHOWN_COUNT; ++i) {
    const tasks::Usage u = tasks::usage(SHOWN[i]);
    if (!u.alive) {
      snprintf(detail, sizeof(detail), "%s · %s", u.budget->what, tr(STR_MEMORY_STOPPED));
      y = rowAt(y, listui::ROW2_H, {.title = u.budget->name, .detail = detail, .meta = "—", .rule = true});
    } else {
      // Lo que se muestra a la derecha es lo USADO contra lo declarado: es el
      // número con el que se decide si un presupuesto está bien puesto.
      formatBytes(left, sizeof(left), u.usedBytes);
      formatBytes(right, sizeof(right), u.budget->stack);
      char freeText[32];
      formatBytes(freeText, sizeof(freeText), u.freeBytes);
      snprintf(detail, sizeof(detail), "%s · %s %s", u.budget->what, freeText, tr(STR_MEMORY_FREE));
      char meta[48];
      snprintf(meta, sizeof(meta), "%s / %s", left, right);
      y = rowAt(y, listui::ROW2_H, {.title = u.budget->name, .detail = detail, .meta = meta, .rule = true});
    }
  }
  y += listui::GAP;

  // --- Batería ------------------------------------------------------------
  // El AXP2101 no tiene registro de corriente, así que no hay miliamperios que
  // mostrar: lo que se muestra es la pendiente real del porcentaje contra el
  // reloj, que es lo que de verdad contesta "cuánto dura".
  {
    const batterylog::Drain d = batterylog::measure();
    y = header(y, tr(STR_MEMORY_BATTERY), nullptr);
    if (!d.valid) {
      y = rowAt(y, listui::ROW1_H, {.title = tr(STR_MEMORY_BATTERY_WAIT), .rule = true});
    } else {
      char title[96];
      char meta[48];
      snprintf(title, sizeof(title), "%.1f %%/h  ·  %s %.0f h (%.1f %s)", d.pctPerHour, tr(STR_MEMORY_BATTERY_LEFT),
               d.hoursLeft, d.hoursLeft / 24.0f, tr(STR_MEMORY_BATTERY_DAYS));
      snprintf(meta, sizeof(meta), "%s %.1f h", tr(STR_MEMORY_BATTERY_WINDOW), d.hours);
      y = rowAt(y, listui::ROW1_H, {.title = title, .meta = meta, .rule = true});
    }
    y += listui::GAP;  // rowAt ya sumó el alto de la fila
  }

  // --- Reposo -------------------------------------------------------------
  y = header(y, tr(STR_MEMORY_REST), nullptr);
  if (IDLE_SLEEP.cycles() == 0) {
    y = rowAt(y, listui::ROW1_H, {.title = tr(STR_MEMORY_REST_NEVER), .rule = false});
  } else {
    formatDuration(left, sizeof(left), IDLE_SLEEP.restedMs());
    snprintf(detail, sizeof(detail), "%lu %s · %s", (unsigned long)IDLE_SLEEP.cycles(), tr(STR_MEMORY_CYCLES), left);
    y = rowAt(y, listui::ROW1_H, {.title = detail, .rule = false});
  }

  y += listui::GAP;

  // --- Panel ---------------------------------------------------------------
  // Cuánto tarda cada forma de refresco EN ESTA PLACA. Sin esto no hay manera
  // de saber si una vuelta de página lenta es la onda parcial del panel o una
  // limpieza que se coló: el coordinador decide bien, pero la onda la pone el
  // vidrio. Sólo cuenta los refrescos bloqueantes (el asíncrono lo espera el
  // lector, no el que refresca).
  {
    const PanelRefreshCoordinator& coord = renderer.refreshCoordinator();
    y = header(y, tr(STR_MEMORY_PANEL), nullptr);
    const HalDisplay::RefreshMode modos[] = {HalDisplay::FAST_REFRESH, HalDisplay::HALF_REFRESH,
                                             HalDisplay::FULL_REFRESH};
    bool alguno = false;
    for (const HalDisplay::RefreshMode modo : modos) {
      const PanelRefreshCoordinator::ModeStat& st = coord.stat(modo);
      if (st.n == 0) continue;
      alguno = true;
      char title[64];
      char meta[48];
      snprintf(title, sizeof(title), "%s  ·  %lu ms", PanelRefreshCoordinator::modeName(modo),
               static_cast<unsigned long>(st.avgMs()));
      snprintf(meta, sizeof(meta), "%lu / max %lu ms", static_cast<unsigned long>(st.n),
               static_cast<unsigned long>(st.maxMs));
      y = rowAt(y, listui::ROW1_H, {.title = title, .meta = meta, .rule = true});
    }
    if (!alguno) {
      y = rowAt(y, listui::ROW1_H, {.title = tr(STR_MEMORY_PANEL_NONE), .rule = false});
    }
  }

  // Las dos bandas que el contenido desplazado no puede invadir: el cabezal
  // arriba y la barra de botones abajo. Se tapan con blanco y recién ahí se
  // dibuja lo que va en ellas.
  const int anchoPantalla = renderer.getScreenWidth();
  renderer.fillRect(0, 0, anchoPantalla, viewTop, false);
  renderer.fillRect(0, viewBottom, anchoPantalla, renderer.getScreenHeight() - viewBottom, false);
  renderer.drawText(UI_14_FONT_ID, x, listui::SIDE, tr(STR_MEMORY_TITLE), true, EpdFontFamily::BOLD);
  renderer.fillRect(x, tituloBase, w, 1, true);

  // El alto total es lo que permite no pasarse del final al desplazar.
  contentH = y - viewTop;

  const bool hayMas = contentH > (viewBottom - viewTop);
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", hayMas ? tr(STR_DIR_UP) : "", hayMas ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  painted = now;
  lastPaint = millis();
}
