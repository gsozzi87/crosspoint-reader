#include "ActivityManager.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <esp_system.h>  // esp_restart (REV-099)

#include <algorithm>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "TaskConfig.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/HubActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/BmpViewerActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
  // REV-097: el NÚCLEO también sale de `TaskConfig`. Estaba recalculado acá con
  // `configNUM_CORES`, que daba lo mismo por casualidad pero rompía la promesa
  // del archivo: "núcleo, prioridad y stack en un solo lugar". Dos fuentes de
  // la misma decisión se separan solas — ya pasó con las rutas protegidas en
  // 1.5.91 y con el índice de las filas en REV-088.
  const BaseType_t renderTaskCore = tasks::budget(tasks::Id::Render).core;
  const BaseType_t ok = xTaskCreatePinnedToCore(&renderTaskTrampoline, tasks::RENDER_NAME, tasks::RENDER_STACK, this,
                                                tasks::RENDER_PRIO, &renderTaskHandle,
                                                renderTaskCore  // long renders/cover decodes off CPU 0's idle watchdog
  );
  // REV-099: `assert()` NO es un mecanismo operativo. En un build sin
  // `NDEBUG` termina en un abort opaco, y en uno con `NDEBUG` la comprobación
  // desaparece entera y se sigue con un handle nulo — o sea que el modo de
  // fallo depende de una macro del build. Sin tarea de render el aparato no
  // puede pintar nada, así que se deja dicho y se reinicia: un arranque
  // limpio tiene chances de conseguir el heap; seguir, ninguna.
  if (ok != pdPASS || renderTaskHandle == nullptr) {
    log_e("[ACT] no se pudo crear la tarea de render (%u B de stack): se reinicia",
          static_cast<unsigned>(tasks::RENDER_STACK));
    delay(200);  // que la línea alcance a salir por el cable
    esp_restart();
  }
}

void ActivityManager::renderTaskTrampoline(void* param) {
  tasks::attach(tasks::Id::Render);  // para medir su stack (Ajustes -> Memoria)
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode is a global output polarity applied to every activity.
      // The sleep screen forces normal polarity itself (SleepActivity).
      display.setInverted(SETTINGS.screenInverted != 0);
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Tap-first control-center entry: a tap on the status-bar band of the
    // top-level tab screens opens it, mirroring the top-edge swipe (which some
    // panels' etched glass makes unreliable). The reader keeps its clean page
    // (no status bar there to tap). Touch boards only, like the swipe itself.
    bool statusBarTap = false;
    if (mappedInput.hasTouch() &&
        (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
         currentActivity->name == "Settings" || currentActivity->name == "NetworkModeSelection")) {
      int tx = 0;
      int ty = 0;
      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44;
    }
    if (currentActivity->name != "FrontlightPanel" && (statusBarTap || mappedInput.wasLightPanelGesture())) {
      pushActivity(makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

// REV-071: el teardown del ciclo de vida para el sueño profundo, sin crear
// ninguna pantalla nueva.
//
// `goToSleep()` no sirve para esto en la ws397: deja el cambio PENDIENTE y el
// `onExit()` de la pantalla saliente corre recién cuando `loop()` procesa ese
// pendiente — y acá `loop()` no se llama nunca más, porque lo que sigue es
// `esp_deep_sleep_start()`. Procesar el pendiente tampoco alcanzaría: el
// `onEnter()` de `SleepActivity` PINTA la pantalla de sueño del SDK, que en
// esta placa no se pinta a propósito desde 1.5.74 (la tapa entera el fondo
// informativo, y pintar las dos cuesta dos refrescos completos).
//
// Así que esto hace exactamente lo que falta y nada más: correr el `onExit()`
// de la pantalla de turno y de toda la pila, y tirar lo que hubiera quedado
// pendiente — al despertar de un sueño profundo se arranca de cero igual.
void ActivityManager::tearDownForSleep() {
  RenderLock lock;
  exitActivity(lock);
  while (!stackActivities.empty()) {
    stackActivities.back()->onExit();
    stackActivities.pop_back();
  }
  pendingActivity.reset();
  pendingAction = PendingAction::None;
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  if (!newActivity) {
    LOG_ERR("ACT", "OOM: replacement activity was not created");
    return;
  }
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer(const bool hideUsbDrive) {
  replaceActivity(makeUniqueNoThrow<CrossPointWebServerActivity>(renderer, mappedInput, hideUsbDrive));
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

void ActivityManager::goToSettings() { replaceActivity(makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(makeUniqueNoThrow<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(makeUniqueNoThrow<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }

  if (FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }

  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (activity) {
    replaceActivity(std::move(activity));
  }
}

void ActivityManager::goToSleep(bool fromTimeout, bool render) {
  replaceActivity(makeUniqueNoThrow<SleepActivity>(renderer, mappedInput, fromTimeout));
  // Important: sleep screen must be rendered immediately, the caller will go to
  // sleep right after this returns. Salvo que el llamador vaya a pintar él otra
  // cosa encima (ws397: el fondo de pantalla con información).
  if (render) loop();
}

void ActivityManager::goToBoot() { replaceActivity(makeUniqueNoThrow<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(makeUniqueNoThrow<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::WS397) {
    // ws397: the hub is home. The classic home stays the parent of the
    // library screens (browser, recents, OPDS, transfer) so Back from them
    // lands where it left; Settings and everything else return to the hub.
    //
    // REV-088: **la transferencia ya NO cuelga de la home clásica en esta
    // placa.** Su fila se escondió (`HomeActivity::showsFileTransfer()`) y la
    // puerta canónica pasó a ser Ajustes -> Archivos, así que volver a la home
    // clásica dejaría el cursor en la primera fila de una pantalla por la que
    // no se pasó. Vuelve al hub, como Ajustes.
    if (initialMenuItem == HomeMenuItem::NONE || initialMenuItem == HomeMenuItem::SETTINGS_MENU ||
        initialMenuItem == HomeMenuItem::FILE_TRANSFER) {
      replaceActivity(makeUniqueNoThrow<HubActivity>(renderer, mappedInput, cleanInitialRefresh));
      return;
    }
  }
  replaceActivity(makeUniqueNoThrow<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh));
}

void ActivityManager::goToClassicHome(HomeMenuItem initialMenuItem) {
  replaceActivity(makeUniqueNoThrow<HomeActivity>(renderer, mappedInput, initialMenuItem, false));
}
void ActivityManager::goToCrashReport() { replaceActivity(makeUniqueNoThrow<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (!activity) {
    LOG_ERR("ACT", "OOM: pushed activity was not created");
    return;
  }
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

const char* ActivityManager::currentActivityName() const {
  return currentActivity ? currentActivity->name.c_str() : "";
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::allowsBackgroundSync() const {
  if (currentActivity && !currentActivity->allowsBackgroundSync()) return false;
  return std::all_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->allowsBackgroundSync(); });
}

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
