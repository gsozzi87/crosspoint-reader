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

  // El medidor de OK mantenido. Se mira `isPressed` y no `wasReleased` a
  // propósito: `wasLongPressed()` se come la soltada siguiente
  // (`suppressNextRelease`), así que si el evento llega, el release nunca
  // aparece y el medidor se quedaría esperando para siempre.
  const bool down = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (down) {
    // getHeldTime() es de "cualquier botón", no por botón: acá vale porque la
    // pantalla no usa ningún otro y lo único apretado es OK.
    okHoldMs = mappedInput.getHeldTime();
    if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, OK_HOLD_TEST_MS)) okFired = true;
  } else if (okDown) {
    lastOkHoldMs = okHoldMs;
    lastOkFired = okFired;
    sawOkHold = true;
    okFired = false;
    okHoldMs = 0;
    requestUpdate();  // se soltó: hay resultado nuevo que mostrar
  }
  okDown = down;

  if (millis() - lastPaint < REFRESH_MS) return;
  // La regla del panel manda: se repinta sólo si algún número se movió de
  // verdad. Un contador que cambia por 16 bytes deja fantasma y no dice nada.
  if (worthRepainting(painted, take())) requestUpdate();
  lastPaint = millis();
}

void TaskStatsActivity::render(RenderLock&&) {
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  const Snapshot now = take();
  char left[64];
  char right[64];
  char detail[96];

  renderer.clearScreen();

  const int hUi14 = renderer.getTextHeight(UI_14_FONT_ID);
  renderer.drawText(UI_14_FONT_ID, x, listui::SIDE, tr(STR_MEMORY_TITLE), true, EpdFontFamily::BOLD);
  renderer.fillRect(x, listui::SIDE + hUi14 + listui::GAP, w, 1, true);
  int y = listui::SIDE + hUi14 + listui::GAP + 1 + listui::GAP;

  // --- Heap ---------------------------------------------------------------
  // La memoria interna es la que se acaba primero (el TLS y los buffers de I2S
  // no pueden vivir en PSRAM), así que va primera y con el mínimo histórico al
  // lado: el promedio no sirve, lo que importa es el peor momento.
  formatBytes(left, sizeof(left), now.internalFree);
  y = listui::sectionHeader(renderer, x, y, w, tr(STR_MEMORY_INTERNAL), left);
  formatBytes(left, sizeof(left), now.internalMin);
  formatBytes(right, sizeof(right), now.internalBlock);
  snprintf(detail, sizeof(detail), "%s: %s  ·  %s: %s", tr(STR_MEMORY_MIN), left, tr(STR_MEMORY_LARGEST), right);
  listui::row(renderer, x, y, w, listui::ROW1_H, {.title = detail, .rule = true});
  y += listui::ROW1_H + listui::GAP;

  formatBytes(left, sizeof(left), now.psramFree);
  y = listui::sectionHeader(renderer, x, y, w, tr(STR_MEMORY_PSRAM), left);
  formatBytes(right, sizeof(right), now.psramBlock);
  snprintf(detail, sizeof(detail), "%s: %s", tr(STR_MEMORY_LARGEST), right);
  listui::row(renderer, x, y, w, listui::ROW1_H, {.title = detail, .rule = true});
  y += listui::ROW1_H + listui::GAP;

  // --- Tareas -------------------------------------------------------------
  y = listui::sectionHeader(renderer, x, y, w, tr(STR_MEMORY_TASKS), nullptr);
  for (size_t i = 0; i < SHOWN_COUNT; ++i) {
    const tasks::Usage u = tasks::usage(SHOWN[i]);
    if (!u.alive) {
      snprintf(detail, sizeof(detail), "%s · %s", u.budget->what, tr(STR_MEMORY_STOPPED));
      listui::row(renderer, x, y, w, listui::ROW2_H,
                  {.title = u.budget->name, .detail = detail, .meta = "—", .rule = true});
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
      listui::row(renderer, x, y, w, listui::ROW2_H,
                  {.title = u.budget->name, .detail = detail, .meta = meta, .rule = true});
    }
    y += listui::ROW2_H;
  }
  y += listui::GAP;

  // --- OK mantenido -------------------------------------------------------
  y = listui::sectionHeader(renderer, x, y, w, tr(STR_MEMORY_OK_HOLD), nullptr);
  if (!sawOkHold) {
    listui::row(renderer, x, y, w, listui::ROW1_H, {.title = tr(STR_MEMORY_OK_HOLD_HINT), .rule = true});
  } else {
    char meta[32];
    snprintf(meta, sizeof(meta), "%lu ms", lastOkHoldMs);
    // tr() es una macro que antepone StrId::, así que el ternario va afuera.
    const char* veredicto =
        lastOkFired ? tr(STR_MEMORY_EVENT_ARRIVED) : tr(STR_MEMORY_EVENT_LOST);
    listui::row(renderer, x, y, w, listui::ROW1_H, {.title = veredicto, .meta = meta, .rule = true});
  }
  y += listui::ROW1_H + listui::GAP;

  // --- Reposo -------------------------------------------------------------
  y = listui::sectionHeader(renderer, x, y, w, tr(STR_MEMORY_REST), nullptr);
  if (IDLE_SLEEP.cycles() == 0) {
    listui::row(renderer, x, y, w, listui::ROW1_H, {.title = tr(STR_MEMORY_REST_NEVER), .rule = false});
  } else {
    formatDuration(left, sizeof(left), IDLE_SLEEP.restedMs());
    snprintf(detail, sizeof(detail), "%lu %s · %s", (unsigned long)IDLE_SLEEP.cycles(), tr(STR_MEMORY_CYCLES), left);
    listui::row(renderer, x, y, w, listui::ROW1_H, {.title = detail, .rule = false});
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  painted = now;
  lastPaint = millis();
}
