#include <Arduino.h>
#include <ws397_version.h>  // ws397: build number lives here, not in a -D flag
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <ServerCredentialStore.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "HubStore.h"
#include "activities/home/PhotosActivity.h"
#include "activities/home/ReminderAlertActivity.h"
#include "activities/home/TimerActivity.h"
#include "activities/home/VoiceActivity.h"
#include "util/DeviceLog.h"
#include <esp_sleep.h>
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/AudioTestActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "music/MusicPlayer.h"
#include "voice/VoiceRecorder.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;

// ws397: tiempos del mantenido de OK/encendido (ver handlePowerHold más abajo).
constexpr unsigned long POWER_HOLD_ACTION_MS = 600;   // desde acá se ve la barrita del apagado
constexpr unsigned long POWER_HOLD_WARN_MS = 2200;    // segundo cartel: ya casi apaga
constexpr unsigned long POWER_HOLD_SLEEP_MS = 3000;   // acá se apaga
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if FREEINK_CAP_TOUCH
static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;

  // A software reset does not cycle externally powered touch/frontlight rails.
  // Shut down the network stack in place so those peripherals retain state.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
  return true;
}
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s by power-button double-click", lightOn ? "on" : "off");
  return true;
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// ws397: the RTC INT pin (GPIO45) is not an RTC GPIO, so a reminder cannot wake
// the chip through the PCF85063 alarm. The deep-sleep timer does it instead:
// armed to the next pending reminder (from the hub cache), the boot path then
// shows ReminderAlertActivity. Needs the RTC set (server clock or NTP).
static void armReminderWake(const bool quiet = false) {
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) {
    if (!quiet) LOG_ERR("MAIN", "no clock: nothing armed, the timer will not ring asleep");
    return;
  }
  time_t due = HUB_STORE.nextDueAt(now);
  // A running timer wakes the device too, and wins when it fires first.
  if (HUB_STORE.timerEndAt > 0 && (due == 0 || HUB_STORE.timerEndAt < due)) due = HUB_STORE.timerEndAt;
  // Algo ya vencido (venció en otra pantalla o mientras se apagaba) tiene que
  // despertar al toque, no quedarse mudo para siempre.
  if (due == 0) {
    const HubStore::Reminder* overdue = HUB_STORE.dueReminder(now);
    if (!overdue) return;
    due = now;
  }
  uint64_t seconds = due > now ? static_cast<uint64_t>(due - now) : 0;
  if (seconds < 5) seconds = 5;
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  if (!quiet) LOG_INF("MAIN", "Reminder wake in %llu s", (unsigned long long)seconds);
}

// Todo camino de deep sleep pasa por acá: si alguno se olvida de armar el
// despertador, el temporizador y los recordatorios quedan mudos hasta que el
// usuario apriete un botón (pasaba en el re-sleep por wake espurio del botón).
static void sleepNow() {
  // La música no sobrevive al deep sleep: cortarla acá deja el códec y el I2S
  // en un estado conocido antes de apagar.
  MUSIC.stop();
  armReminderWake(/*quiet=*/true);
  powerManager.startDeepSleep(gpio);
}

// ws397: el temporizador y los recordatorios tienen que sonar aunque el aparato
// esté despierto en otra pantalla. Antes solo los miraba el tick del hub y el
// arranque después de dormir, así que un temporizador vencido en Notas, Agenda o
// Ajustes no sonaba nunca. Se dispara sobre las pantallas tranquilas; el lector y
// las que usan red o audio se dejan en paz (ahí manda el wake por deep sleep).
constexpr unsigned long DOUBLE_BACK_MS = 500;  // ventana del doble toque de Atrás

// Pantallas "tranquilas" (ver abajo) pero con el micrófono abierto NO lo son:
// Notas, Agenda y Calendario ahora graban, y un recordatorio que se abriera
// encima dejaba el micrófono colgado y la toma perdida.
static bool busyRecording() { return VoiceRecorder::anyRecording(); }

static bool isCalmScreen(const char* name) {
  // Pantallas tranquilas: ahi suenan los recordatorios y el temporizador, y
  // anda el doble Atras para hablar. Calendar y Trip son listas quietas igual
  // que Agenda, asi que entran (si no, en el calendario no sonaria una alarma).
  static const char* CALM[] = {"Hub", "Home", "Agenda", "Notes", "Settings", "Weather", "Calendar", "Trip", "Music"};
  for (const char* n : CALM) {
    if (strcmp(name, n) == 0) return true;
  }
  return false;
}

// Atajo de voz: DOS toques de Atrás seguidos. UP/DOWN es una palanca física
// (arriba XOR abajo, nunca las dos), OK es el botón de encendido y un Atrás
// mantenido ya sincroniza o actualiza según la pantalla; el doble toque es lo
// único que queda libre. Desde cualquier pantalla: el primer toque vuelve al
// hub y el segundo abre Hablar (en el hub, Atrás no hace nada).
static void checkVoiceShortcut() {
  static unsigned long lastBackRelease = 0;
  if (!mappedInputManager.wasReleased(MappedInputManager::Button::Back)) return;
  const unsigned long now = millis();
  const bool isDouble = lastBackRelease != 0 && now - lastBackRelease <= DOUBLE_BACK_MS;
  lastBackRelease = isDouble ? 0 : now;  // el segundo toque cierra la ventana
  if (!isDouble) return;
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) return;
  if (busyRecording()) return;
  const char* name = activityManager.currentActivityName();
  if (!isCalmScreen(name)) return;
  LOG_INF("MAIN", "PTT shortcut from %s", name);
  activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInputManager));
}

static void checkTimeAlarms() {
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) return;
  if (busyRecording()) return;
  if (!isCalmScreen(activityManager.currentActivityName())) return;
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) return;
  if (HUB_STORE.timerRunning() && HUB_STORE.timerEndAt <= now) {
    activityManager.pushActivity(std::make_unique<TimerActivity>(renderer, mappedInputManager, 0, /*resumeFired=*/true));
    return;
  }
  if (const HubStore::Reminder* due = HUB_STORE.dueReminder(now)) {
    activityManager.pushActivity(
        std::make_unique<ReminderAlertActivity>(renderer, mappedInputManager, due->id, due->title, due->when));
  }
}

// ws397: fondo de pantalla. La foto que se eligió en Ajustes → Fondo de pantalla
// se pinta acá, encima de la pantalla de sueño y con la SD todavía montada (más
// adelante `Storage.prepareForDeepSleep()` la desmonta y `display.deepSleep()`
// apaga el panel, así que este es el último momento posible). Sin foto elegida,
// o si el archivo no está o no se puede leer, queda la pantalla de sueño de
// siempre: nunca se cuelga el sueño por esto.
static void paintWallpaperForSleep() {
  if (HUB_STORE.wallpaperPath.empty()) return;
  if (!Storage.exists(HUB_STORE.wallpaperPath.c_str())) {
    LOG_ERR("MAIN", "fondo de pantalla: no está %s", HUB_STORE.wallpaperPath.c_str());
    return;
  }
  if (!PhotosActivity::drawFullScreenPhoto(renderer, HUB_STORE.wallpaperPath)) {
    LOG_ERR("MAIN", "fondo de pantalla: no se pudo pintar %s", HUB_STORE.wallpaperPath.c_str());
  }
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Después de guardar el cuadro de Quick Resume (ese tiene que ser la pantalla
  // anterior, no el fondo) y antes de apagar el panel.
  paintWallpaperForSleep();

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  // Armar y loguear con la SD todavía montada: después de prepareForDeepSleep()
  // el log se escribe sobre un filesystem desmontado y se pierde.
  armReminderWake();
  LOG_DBG("MAIN", "Entering deep sleep");
  devlog::close();
  Storage.prepareForDeepSleep();

  sleepNow();
}

// ws397: el botón de encendido es el MISMO OK (InputStyle::DigitalConfirmPowerHold),
// así que hasta ahora cualquier mantenido de más de 400 ms dormía el aparato y la
// pulsación larga de OK no servía para nada más. Ahora el mantenido se reparte por
// tiempos:
//   toque corto ................. confirmar (lo resuelve el SDK: menos de
//                                 CONFIRM_POWER_HOLD_MS = 400 ms)
//   soltar antes de 1,2 s ....... nada, se puede arrepentir sin consecuencias
//   soltar entre 1,2 s y 3 s .... Hablar, el mismo PTT del doble toque de Atrás
//   mantener 3 s ................ apagar
// El umbral de los 400 ms lo maneja el SDK y NO se toca: es el que decide entre
// confirmar y encendido. Lo que cambia es cuándo duerme, que siempre estuvo acá.
// 1.5.43: el usuario SI quiere la barrita, pero solo para apagar: "el boton PWR
// es el que al mantenerlo apretado tiene que mostrar esa barrita de carga de 3s
// para apagarse". O sea que vuelve el indicador, pero SIN el tramo del medio que
// abria Hablar (eso era idea mia y terminaba apagando cuando no correspondia).
// Mantener OK 3 s apaga, y mientras tanto se ve cuanto falta.
static bool usePowerHoldTiers() {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::WS397 &&
         SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP;
}


// Cartel del mantenido, para que se vea que algo está pasando y hasta dónde hay
// que seguir apretando. Se pinta ENCIMA de lo que haya (no se limpia la pantalla)
// y son dos pasadas como mucho por gesto: cada repintada de tinta electrónica
// cuesta medio segundo, y la regla del panel es no gastar parciales al pedo.
static void drawPowerHoldBanner(const unsigned long held, const bool aboutToSleep) {
  RenderLock lock;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int boxW = screenW - 72;
  const int boxH = 118;
  const int x = (screenW - boxW) / 2;
  const int y = screenH / 2 - boxH / 2;

  renderer.fillRoundedRect(x, y, boxW, boxH, 16, Color::White);
  renderer.drawRoundedRect(x, y, boxW, boxH, 3, 16, true);

  const StrId what = aboutToSleep ? StrId::STR_PWR_HOLD_SLEEPING : StrId::STR_PWR_HOLD_OFF;
  renderer.drawCenteredText(UI_12_FONT_ID, y + 30, I18N.get(what), true, EpdFontFamily::BOLD);

  // Barra: cuánto falta para el apagado.
  const int barX = x + 24;
  const int barW = boxW - 48;
  const int barY = y + 76;
  constexpr int barH = 16;
  renderer.drawRect(barX, barY, barW, barH, 2, true);
  const int filled = static_cast<int>(barW * held / POWER_HOLD_SLEEP_MS);
  if (filled > 4) renderer.fillRect(barX + 2, barY + 2, filled - 4, barH - 4, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Devuelve true cuando se quedó con la pasada del loop (hay cartel en pantalla o
// ya se disparó la acción), así la Activity de abajo no repinta encima.
static bool handlePowerHold(const bool gateOpen) {
  static int bannerStage = 0;  // 0 sin cartel, 1 con el cartel, 2 avisando el apagado

  const bool pressed = gpio.isPressed(HalGPIO::BTN_POWER);
  const unsigned long held = gpio.getPowerButtonHeldTime();

  if (pressed) {
    // Sin el permiso de dormir (recién despertó) o con ABAJO apretado (captura
    // de pantalla) el mantenido no es nuestro.
    if (!gateOpen || gpio.isPressed(HalGPIO::BTN_DOWN)) return false;
    if (held >= POWER_HOLD_SLEEP_MS) {
      LOG_DBG("MAIN", "Power button held %lums, sleeping", held);
      bannerStage = 0;
      enterDeepSleep();
      // No se llega: enterDeepSleep() termina en esp_deep_sleep_start.
      return true;
    }
    if (bannerStage == 0 && held >= POWER_HOLD_ACTION_MS) {
      bannerStage = 1;
      drawPowerHoldBanner(held, false);
    } else if (bannerStage == 1 && held >= POWER_HOLD_WARN_MS) {
      bannerStage = 2;
      drawPowerHoldBanner(held, true);  // segunda y última repintada: la tinta cuesta
    }
    return bannerStage != 0;
  }

  if (bannerStage == 0) return false;
  // Solto antes de los 3 s: no pasa nada, solo se saca el cartel. El atajo de
  // voz vive en el doble toque de Atras, no aca.
  bannerStage = 0;
  activityManager.requestUpdate();
  return true;
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  devlog::begin();  // from here every LOG_* line also goes to the SD
  setLogSink(&devlog::write);
  SERVER_STORE.loadFromFile();
  // El aparato tiene identidad propia desde el primer arranque: si no hay token
  // guardado se genera uno al azar y se persiste. Después se vincula a una
  // cuenta desde la web con el código que muestra Ajustes -> Vincular con mi
  // cuenta; nunca hay que escribir un token de 64 caracteres con la palanca.
  SERVER_STORE.ensureToken();
  HUB_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  // ws397: la ganancia del micrófono se calibra en Ajustes -> Prueba de audio y
  // vale para todo el aparato (el dictado incluido), así que se aplica acá.
  micgain::loadAndApply();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots preserve the live state so they do not unexpectedly go dark.
  const bool restoreLightOn = SETTINGS.frontlightOn != 0 && (SETTINGS.frontlightRestoreOnWake != 0 || isSilentReboot);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        devlog::close();
        Storage.prepareForDeepSleep();
        sleepNow();
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_WS397
      // WS397 is flashed over native USB like the EEGO A4: keep it awake during
      // bring-up so a post-flash reset never lands in deep sleep.
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      devlog::close();
      Storage.prepareForDeepSleep();
      sleepNow();
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  // ws397: a deep-sleep timer wake is a reminder coming due (see armReminderWake).
  const bool isReminderWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  const HubStore::Reminder* dueReminder = nullptr;
  bool timerFired = false;
  {
    // En CUALQUIER arranque: si el temporizador venció o hay un recordatorio en
    // hora, suena. Antes solo se miraba cuando la causa del wake era el timer,
    // así que despertar con OK dejaba todo mudo.
    time_t nowEpoch = 0;
    bool haveClock = false;
    for (int i = 0; i < 3 && !haveClock; ++i) {  // el bus I²C es compartido: reintentar
      haveClock = halClock.getEpochUtc(nowEpoch);
      if (!haveClock) delay(20);
    }
    if (haveClock) {
      dueReminder = HUB_STORE.dueReminder(nowEpoch + 30);
      timerFired = HUB_STORE.timerEndAt > 0 && HUB_STORE.timerEndAt <= nowEpoch + 30;
    } else if (isReminderWake) {
      // Despertó por el timer y el RTC no contestó: reintentar en un minuto en
      // vez de dormir sin nada armado (quedaría mudo para siempre).
      LOG_ERR("MAIN", "timer wake without a clock: retrying in 60 s");
      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      devlog::close();
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
    }
    if (isReminderWake && !dueReminder && !timerFired) {
      // Woke early or the reminder went away (ticked from the phone): straight back to sleep.
      LOG_INF("MAIN", "Timer wake with nothing due, sleeping again");
      devlog::close();
      Storage.prepareForDeepSleep();
      sleepNow();
    }
  }
  const BootResume resume = isSilentReboot                             ? BootResume::Silent
                            : (isPersistedSleepWake || isReminderWake) ? BootResume::SplashlessWake
                                                                       : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        // The first Home/Reader paint is followed by an explicit clean refresh
        // because the panel still physically shows the sleep image.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (timerFired) {
    // The timer ran out while asleep: same alert screen, no server round trip.
    activityManager.replaceActivity(std::make_unique<TimerActivity>(renderer, mappedInputManager, 0, /*resumeFired=*/true));
  } else if (dueReminder) {
    activityManager.replaceActivity(std::make_unique<ReminderAlertActivity>(
        renderer, mappedInputManager, dueReminder->id, dueReminder->title, dueReminder->when));
  } else if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep() || MUSIC.isActive()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only after the frontlight
  // double-click window expires without a second click.
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && BoardConfig::isX4Pro() &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    mappedInputManager.setPowerConfirmClickFrame(true);
  }
#endif

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  const bool powerGateOpen = powerReleasedSinceWake && millis() >= allowSleepAt;

  if (usePowerHoldTiers()) {
    // ws397: el mantenido se reparte entre Hablar y apagar (handlePowerHold).
    if (handlePowerHold(powerGateOpen)) {
      delay(10);  // con el cartel en pantalla no hace falta girar en vacío
      return;
    }
  } else if (powerGateOpen && gpio.isPressed(HalGPIO::BTN_POWER) &&
             gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

  // La música vive fuera de la Activity (MusicPlayer): esto es lo que engancha
  // la pista siguiente cuando termina la anterior, esté abierto el reproductor
  // o esté el usuario en el hub.
  MUSIC.pump();

  checkVoiceShortcut();

  static unsigned long lastAlarmCheck = 0;
  if (millis() - lastAlarmCheck >= 5000) {
    lastAlarmCheck = millis();
    checkTimeAlarms();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
