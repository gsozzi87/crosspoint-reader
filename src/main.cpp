#include <Arduino.h>
#include <BoardConfig.h>
#include <DrawScope.h>
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
#include <NetPump.h>
#include <PowerManager.h>
#include <SPI.h>
#include <ServerClient.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <ws397_version.h>  // ws397: build number lives here, not in a -D flag

#include "Memory.h"
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif

#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HubStore.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/home/ReminderAlertActivity.h"
#include "activities/home/SetupActivity.h"
#include "activities/home/SleepScreen.h"
#include "activities/home/TimerActivity.h"
#include "activities/home/VoiceActivity.h"
#include "util/DeviceLog.h"
#if SOC_PM_SUPPORT_EXT1_WAKEUP
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_system.h>
#endif
// Fuera del #if: `sleepNow()` lo llama sin guarda de preprocesador (la guarda
// es `BoardConfig::isWS397()`, de ejecución), así que en una placa sin EXT1
// —el C3 del env `default`— el include faltaba y no compilaba.
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "TaskConfig.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/AudioTestActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "input/MotionInput.h"
#include "music/MusicPlayer.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "sync/Sync.h"
#include "util/BatteryLog.h"
#include "util/ButtonNavigator.h"
#include "util/CardLayout.h"
#include "util/CodecSleep.h"
#include "util/IdleSleep.h"
#include "util/LoopWatchdog.h"
#include "util/NetPumpHooks.h"
#include "util/PowerKey.h"
#include "util/RescueState.h"
#include "util/RtcAlarm.h"
#include "util/ScreenshotUtil.h"
#include "util/Shtc3.h"
#include "util/SleepRequest.h"
#include "util/TempSweep.h"
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

// ws397: PWR hold timings (see handlePowerHold below). The key is the AXP2101
// PWRKEY decoded by PowerKey, not a GPIO.
// PWR hace UNA sola cosa y se explica en una línea: mantenerlo.
//   antes de 1,2 s ....... no pasa nada (no hay menú, no hay toque corto)
//   a los 1,2 s .......... aparece la barrita
//   soltar con la barrita  SUSPENDE (deep sleep; las alarmas siguen vivas)
//   llegar a los 3 s ..... APAGA (el PMIC corta los rieles)
// Suspender pasa al SOLTAR y no al cruzar el umbral: si durmiera a los 1,2 s
// con el botón abajo, nunca se podría llegar a los 3.
constexpr unsigned long POWER_HOLD_ACTION_MS = 1200;  // aparece la barrita (cuánto falta para apagar)
constexpr unsigned long POWER_HOLD_WARN_MS = 2300;    // el cartel pasa a "Apagando..."
// Cada cuánto se repinta la barrita. El panel no tiene refresco por región expuesto
// (FreeInkDisplay::displayWindow existe pero está marcado EXPERIMENTAL y no sube ni a
// HalDisplay ni a GfxRenderer), así que cada paso es un parcial de pantalla entera de
// ~250 ms: 360 ms es lo más seguido que se puede pedir sin que el loop deje de ver la
// suelta a tiempo. Da cinco pasos entre 1,2 s y 3 s, en vez de los dos saltos de antes.
constexpr unsigned long POWER_HOLD_OFF_MS = 3000;  // apagar de verdad
constexpr unsigned long POWER_HOLD_STEP_MS = 360;  // repintado de la barrita
// Una pulsacion de PWR anclada antes de esto arranco con el aparato: no es un hold.
constexpr unsigned long BOOT_KEY_IGNORE_MS = 3500;
}  // namespace

// A wake hold must never become an in-app button action. Boot may continue
// while the wake key is held; swallow the one release that ends that wake
// gesture. The wake key is the power button on most boards and OK on the
// ws397 (its PWR key sits behind the PMIC and cannot wake the chip), so this
// watches HalGPIO::BTN_CONFIRM there — see wakeKeyIndex().
static bool wakeKeyReleasePending = false;

static uint8_t wakeKeyIndex() { return BoardConfig::isWS397() ? HalGPIO::BTN_CONFIRM : HalGPIO::BTN_POWER; }

// The logical front button the wake key is mapped to (the front buttons can be
// remapped in settings, so physical OK is not always logical Confirm).
static MappedInputManager::Button wakeKeyLogicalButton() {
  const uint8_t hw = wakeKeyIndex();
  if (SETTINGS.frontButtonBack == hw) return MappedInputManager::Button::Back;
  if (SETTINGS.frontButtonLeft == hw) return MappedInputManager::Button::Left;
  if (SETTINGS.frontButtonRight == hw) return MappedInputManager::Button::Right;
  return MappedInputManager::Button::Confirm;
}

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
// SMALL en negrita: mismo advanceY/ascender/descender que la regular (23/18/-5),
// así entra en los mismos renglones sin tocar ningún layout. Además arregla los
// lugares que ya pedían SMALL + BOLD y hasta ahora pintaban regular sin avisar.
EpdFont smallBoldFont(&notosans_8_bold);
EpdFontFamily smallFontFamily(&smallFont, &smallBoldFont);

// UI_14: el escalón de título que faltaba entre UI_12 (alto de mayúscula 17 px)
// y los dígitos de siete segmentos. Solo negrita: la familia se arma con una
// sola cara y el pedido de regular cae en la misma, que es lo correcto para un
// rol que siempre es título.
EpdFont ui14BoldFont(&ubuntu_14_bold);
EpdFontFamily ui14FontFamily(&ui14BoldFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
// Cuántos arranques por temporizador seguidos encontraron el RTC mudo. Va en
// RTC RAM porque cada reintento es un arranque distinto: en una variable normal
// el contador nace en cero cada vez y el tope no existiría. `RTC_DATA_ATTR` (y
// no NOINIT) porque acá sí conviene que un encendido en frío lo ponga en cero.
RTC_DATA_ATTR int clocklessRetries;
constexpr int MAX_CLOCKLESS_RETRIES = 5;

RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
// EL PANEL NO CONTESTA (1.5.108): acá vive el estado del rescate del panel —
// si el ciclo de corriente ya se HIZO (y entonces no se repite) o si se INTENTÓ
// y el PMIC no dejó (y entonces se vuelve a probar, hasta un tope). La cuenta y
// los valores están en util/PanelRescue.h, que es puro y se prueba sin placa.
// Sobrevive a un reinicio y al sueño profundo, no a un corte de energía — que
// es justamente el otro remedio.
RTC_NOINIT_ATTR uint32_t panelRescueMagic;
// REV-061: los rieles no se pudieron cortar al dormir.
//
// `railsOffForSleep()` ya devuelve false cuando la escritura o la relectura
// fallan, pero su resultado se tiraba — y no alcanza con loguearlo, porque para
// cuando se llama el log YA ESTÁ CERRADO (tiene que estarlo: se corta la
// alimentación del panel y del códec justo después). Así que la noticia viaja
// en RTC RAM, que sobrevive al sueño profundo, y se cuenta en el arranque
// siguiente. Sin esto, dormir con el panel y el audio alimentados toda la noche
// —el mismo drenaje que motivó cortar ALDO1-3— no dejaba una sola línea.
static constexpr uint32_t RAILS_STUCK_MAGIC = 0x52414C53;  // "RALS"
RTC_NOINIT_ATTR uint32_t railsStuckMagic;
// REV-070: ya se intentó UNA vez el rescate de los rieles en este encendido.
// Mismo patrón que el rescate del panel: un ciclo de corriente y un reinicio, y
// si al volver sigue sin confirmarse no se insiste — se arranca degradado y el
// log lo dice, que es mejor que un bucle de reinicios.
// REV-070: mismo estado que el rescate del panel y por el mismo motivo — la
// marca se ponía ANTES de llamar al PMIC y el retorno de `railsCycle()` se
// tiraba, así que un fallo de I2C durante el rescate gastaba el único intento
// con el corte SIN HACER. Los valores y la cuenta viven en util/RescueState.h.
RTC_NOINIT_ATTR uint32_t railsRescueMagic;
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

// ws397: el "reinicio silencioso" es un ESP.restart() de verdad, y existe por un
// solo motivo — TLS deja el heap hecho pedazos y el parseo de un EPUB necesita
// bloques contiguos grandes. Pero se llamaba SIEMPRE al salir de cualquier
// pantalla con red: volver de Hablar o de una sincronización reiniciaba el
// aparato, y con el cable puesto eso se ve del otro lado como que el USB se
// desconecta y se vuelve a conectar. Encima cuesta los 2-3 s de arranque.
//
// Reiniciar sólo cuando hace falta: si el bloque contiguo más grande sigue
// siendo holgado, se apaga la red en el lugar y listo. El umbral está por
// encima de lo que necesita abrir un libro, que es el caso que motivó el
// reinicio; por debajo se reinicia como siempre y queda dicho en el log.
constexpr size_t HEAP_BLOCK_OK_BYTES = 96 * 1024;

static bool finishWifiSessionIfHeapIsHealthy() {
  if (!BoardConfig::isWS397()) return false;
  const size_t largest = ESP.getMaxAllocHeap();
  if (largest < HEAP_BLOCK_OK_BYTES) {
    LOG_INF("MAIN", "reinicio: el bloque mayor quedó en %u KB", static_cast<unsigned>(largest / 1024));
    return false;
  }
  WiFi.mode(WIFI_OFF);
  delay(50);
  LOG_INF("MAIN", "red apagada sin reiniciar (bloque mayor %u KB)", static_cast<unsigned>(largest / 1024));
  return true;
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  if (finishWifiSessionIfHeapIsHealthy()) return;
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
// Cuándo hay que volver a estar despierto: lo vencido primero, después lo
// próximo, y el temporizador compitiendo con los dos.
//
// El orden importa y estaba al revés: se preguntaba primero por `nextDueAt`,
// que SÓLO devuelve vencimientos futuros, y recién se miraba lo vencido si no
// había ninguno futuro. Con un recordatorio vencido hace un minuto y otro para
// mañana, el aparato se dormía hasta mañana y el vencido no sonaba nunca. Lo
// vencido gana siempre: ya tendría que haber sonado.
//
// Devuelve 0 si no hay nada que esperar.
static time_t nextWakeInstant(const time_t now) {
  // Vencido: despertar ya (el llamador le pone el piso de segundos).
  if (HUB_STORE.dueReminder(now) != nullptr) return now;
  if (HUB_STORE.timerRunning() && HUB_STORE.timerEndAt <= now) return now;

  time_t due = HUB_STORE.nextDueAt(now);
  if (HUB_STORE.timerRunning() && HUB_STORE.timerEndAt > now && (due == 0 || HUB_STORE.timerEndAt < due)) {
    due = HUB_STORE.timerEndAt;
  }
  return due;
}

static bool reminderWakeArmed = false;  // enterDeepSleep() arms with the log; sleepNow() only fills the gap
// REV-060: si de verdad quedó un TIMER armado. `reminderWakeArmed` no sirve
// para eso: se pone en true también cuando no hay nada que sonar (`due == 0`),
// que es el caso normal. Esto distingue "ya lo intenté" de "hay una fuente de
// despertar puesta", y es lo que decide si hace falta la red de seguridad.
static bool wakeTimerArmed = false;
// REV-073: cuántas veces se intentó armar el despertador del recordatorio en
// este ciclo de sueño. El diseño ya preveía dos intentos (enterDeepSleep con
// log, sleepNow en silencio); esto los cuenta para poder hacer algo distinto en
// el segundo en vez de resignar el vencimiento.
static int reminderRetries = 0;
// REV-073: hay un vencimiento pendiente y NO se pudo dejar armado el timer que
// lo va a hacer sonar. Lo mira `enterDeepSleep()` ANTES de desmontar nada.
static bool reminderWakeMissed = false;
// Cuántas veces seguidas se abortó un sueño por eso. Acotado: a la cuarta se
// duerme igual, porque un aparato que se niega a suspender para siempre es peor
// que una alarma perdida con su línea en el log.
static uint32_t reminderSleepAborts = 0;
static constexpr uint32_t MAX_SLEEP_ABORTS = 3;
static void armReminderWake(const bool quiet = false) {
  if (reminderWakeArmed) return;
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) {
    // Not latched: the second caller gets another go at the shared I2C bus.
    if (!quiet) LOG_ERR("MAIN", "no clock: nothing armed, the timer will not ring asleep");
    return;
  }
  const time_t due = nextWakeInstant(now);
  if (due == 0) {
    // No hay nada que sonar: no hay nada que reintentar tampoco, así que se
    // latchea y listo.
    reminderWakeArmed = true;
    return;
  }
  uint64_t seconds = due > now ? static_cast<uint64_t>(due - now) : 0;
  if (seconds < 5) seconds = 5;
  // REV-073: y TOPE POR ARRIBA, que es lo que faltaba. `esp_sleep_enable_timer_wakeup()`
  // rechaza un plazo fuera de rango, y `nextWakeInstant()` sale de un `dueAt`
  // que puede venir del servidor, de la caché o de un reloj mal puesto: un
  // recordatorio con una fecha absurda era la causa MÁS probable de que la
  // llamada fallara, y todo el manejo de abajo existe para ese fallo. Con el
  // tope, despertar de más cuesta un arranque y recalcular; sin él, el
  // vencimiento se perdía entero.
  constexpr uint64_t MAX_SLEEP_S = 12ULL * 60 * 60;
  if (seconds > MAX_SLEEP_S) seconds = MAX_SLEEP_S;
  // REV-073: el retorno se mira, y la bandera se pone DESPUÉS y sólo si salió
  // bien. Antes se latcheaba arriba de todo: si esta llamada fallaba, el
  // `if (reminderWakeArmed) return;` del principio se comía el único reintento
  // que el diseño tenía previsto — `enterDeepSleep()` llama a esto con log y
  // `sleepNow()` lo repite en silencio como segunda oportunidad, y esa segunda
  // oportunidad quedaba bloqueada justo en el caso para el que existe.
  esp_err_t err = esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  if (err != ESP_OK) {
    // REV-073: el primer fallo deja reintentar (por eso no se latchea la
    // bandera), pero si el SEGUNDO intento también falla el aparato se dormía
    // "como si nada" y el vencimiento se perdía. Antes de resignarlo se prueba
    // con un plazo CORTO: volver en un minuto y recalcular es tarde, pero es
    // muchísimo mejor que no sonar nunca. Sólo se intenta cuando ya hubo un
    // intento previo, para no gastarlo en la primera vuelta.
    if (reminderRetries > 0) {
      constexpr uint64_t REINTENTO_S = 60;
      const esp_err_t err2 = esp_sleep_enable_timer_wakeup(REINTENTO_S * 1000000ULL);
      if (err2 == ESP_OK) {
        reminderWakeArmed = true;
        wakeTimerArmed = true;
        reminderWakeMissed = false;
        reminderSleepAborts = 0;
        LOG_ERR("MAIN",
                "no se pudo armar el despertador del recordatorio (%llu s, err %d) dos veces: se vuelve en "
                "%llu s a recalcularlo en vez de perder el vencimiento",
                (unsigned long long)seconds, (int)err, (unsigned long long)REINTENTO_S);
        return;
      }
      err = err2;
    }
    ++reminderRetries;
    // REV-073: queda ANOTADO que hay un vencimiento sin despertador. Antes esto
    // era sólo una línea de log y el sueño seguía su curso: `ensureSomeWakeSource()`
    // veía que OK puede despertar, se daba por satisfecho, y la alarma se perdía
    // en silencio. Ahora lo mira `enterDeepSleep()` antes de desmontar nada.
    reminderWakeMissed = true;
    LOG_ERR("MAIN", "no se pudo armar el despertador del recordatorio (%llu s, err %d): se reintenta",
            (unsigned long long)seconds, (int)err);
    return;
  }
  reminderWakeArmed = true;
  wakeTimerArmed = true;
  reminderWakeMissed = false;
  reminderSleepAborts = 0;
  if (!quiet) LOG_INF("MAIN", "Reminder wake in %llu s", (unsigned long long)seconds);
}

// Todo camino de deep sleep pasa por acá: si alguno se olvida de armar el
// despertador, el temporizador y los recordatorios quedan mudos hasta que el
// usuario apriete un botón (pasaba en el re-sleep por wake espurio del botón).
// REV-060: NUNCA se entra al sueño profundo sin UNA fuente de despertar.
//
// Los dos casos en que el botón no puede despertar —no hay pin en el perfil, o
// el pin no es RTC GPIO— se detectaban, se anotaban… y se dormía igual. Y el
// timer casi nunca está puesto: `armReminderWake()` sólo lo arma si hay un
// recordatorio o un temporizador pendiente (`due == 0` es el caso normal), así
// que lo habitual es que el ÚNICO despertador sea el botón. Si ese botón no se
// puede armar, el aparato se duerme sin nada: desde afuera está muerto, y la
// única salida es PWR 10 s o sacarle la batería.
//
// El timer no depende de ningún GPIO ni de que el pin sea RTC, así que sirve de
// red. Cinco minutos es el compromiso: bastante corto para que no parezca roto,
// bastante largo para no ser un ciclo de arranques si el defecto es permanente.
//
// Vive en una función porque hay DOS caminos que llaman a `startDeepSleep()`:
// el normal y el reintento de "timer wake sin reloj" del `setup()`. Escrita dos
// veces se separaría, como ya pasó con las rutas protegidas en 1.5.91.
static void ensureSomeWakeSource() {
  const int8_t wakePin = freeink::PowerManager::wakeSourcePin();
  bool elBotonPuedeDespertar = wakePin >= 0;
  if (wakePin < 0) {
    LOG_ERR("MAIN", "no wake pin in the board profile: only the timer can wake the device");
  }
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  else if (!rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(wakePin))) {
    // The SDK refuses to arm it (and says so only on the serial console).
    LOG_ERR("MAIN", "wake pin GPIO%d is not an RTC GPIO: the button will NOT wake the device", wakePin);
    elBotonPuedeDespertar = false;
  }
#endif
  if (elBotonPuedeDespertar || wakeTimerArmed) return;
  constexpr uint64_t RESCATE_S = 300;
  // Y ESTE TAMBIÉN SE COMPRUEBA. La red de REV-060 se apoyaba en una llamada
  // que no miraba su propio retorno: garantizar una fuente de despertar con algo
  // sin verificar no garantiza nada.
  const esp_err_t err = esp_sleep_enable_timer_wakeup(RESCATE_S * 1000000ULL);
  if (err == ESP_OK) {
    wakeTimerArmed = true;
    LOG_ERR("MAIN",
            "!!! OJO: ningun boton puede despertar al aparato. Se arma un temporizador de %llu s para que "
            "vuelva solo; sin esto quedaria muerto hasta PWR 10 s o sacarle la bateria",
            (unsigned long long)RESCATE_S);
    return;
  }
  // NI EL TIMER. Acá ya no queda ninguna fuente, y dormir es el paso que no se
  // puede deshacer: el aparato quedaría muerto hasta PWR 10 s o hasta sacarle la
  // batería. Un reinicio SÍ se deshace solo — el despertar del sueño profundo es
  // un reset igual, así que volver por acá no es peor que volver por allá, y al
  // menos vuelve. Se pierde el estado que no se haya guardado, que a esta altura
  // ya está en la tarjeta.
  LOG_ERR("MAIN",
          "!!! OJO: no se pudo armar NINGUNA fuente de despertar (timer: %d). No se duerme: se reinicia, "
          "que es lo unico de lo que el aparato vuelve solo",
          (int)err);
  devlog::close();
  delay(50);  // que la ultima linea llegue a la tarjeta
  esp_restart();
}

static void sleepNow() {
  // El supervisor del loop no tiene que ver el sueño como un cuelgue: de acá no
  // se vuelve, y lo que sigue (pintar, desmontar, cortar rieles) puede tardar
  // segundos sin que nadie late.
  loopwdt::pause("sueño profundo");
  // La música no sobrevive al deep sleep: cortarla acá deja el códec y el I2S
  // en un estado conocido antes de apagar.
  MUSIC.stop();
  // Lo que quedaba comiendo corriente dormido (1.5.72). Va ACÁ y no en
  // enterDeepSleep() porque este es el único punto por el que pasan TODOS los
  // caminos de sueño: los tres re-sleep del setup() (wake por timer sin nada
  // vencido, wake espurio del botón) no pasan por enterDeepSleep() y se estaban
  // durmiendo con el IMU muestreando a 250 Hz, que es el consumidor más grande
  // de la lista.
  halTiltSensor.deepSleep();   // QMI8658 a dormir; idempotente
  AudioManager::silenceAmp();  // el enable del amplificador (GPIO39) a un nivel definido
  if (BoardConfig::isWS397()) {
    // LO QUE SE COMÍA LA BATERÍA DURMIENDO (1.5.106): 9 % en una noche, o sea
    // del orden de 10 mA con el S3 en sueño profundo (que son microamperios).
    // El firmware no toca los rieles del PMIC, así que Audio_VCC (ES8311) y
    // AudioCTR_VCC/VCC3V3 (NS4150B) siguen a 3,3 V toda la noche; y dos cosas
    // dejaban a esos dos chips ENCENDIDOS de verdad:
    //  1. el ES8311 nunca se apagaba (powerDown() sólo corta un riel por GPIO
    //     que acá no existe): quedaba polarizado como lo dejó la última
    //     reproducción;
    //  2. el enable del NS4150B es GPIO39, que NO es RTC GPIO y en la placa no
    //     tiene resistencia a masa (R74 sin poblar): el LOW de silenceAmp()
    //     dura hasta que esp_sleep_config_gpio_isolate() suelta el pad, y el
    //     amplificador pasa la noche con el enable al aire.
    // El códec se pone en suspensión por I2C y el pin se RETIENE en bajo a
    // través del sueño (gpio_deep_sleep_hold_en lo hace el SDK). setup() lo
    // libera antes de que el audio lo vuelva a manejar.
    codecsleep::es8311Suspend();
    const int8_t amp = BoardConfig::ACTIVE.audio.ampEnable;
    if (amp >= 0) gpio_hold_en(static_cast<gpio_num_t>(amp));
  }
  armReminderWake(/*quiet=*/true);
  // Y LOS RIELES (1.5.107): ALDO1-3 = panel, códec y amplificador, apagados
  // hasta el próximo arranque, donde PowerKey::begin() los enciende antes que
  // nada. Va al final, con el panel ya en su deep sleep y el log cerrado: desde
  // acá hasta esp_deep_sleep_start() no se toca ni la pantalla ni el audio.
  if (BoardConfig::isWS397()) {
    // REV-061: el resultado NO se tira. El log ya está cerrado acá, así que la
    // marca va a RTC RAM y el próximo arranque lo cuenta.
    railsStuckMagic = POWER_KEY.railsOffForSleep() ? 0 : RAILS_STUCK_MAGIC;
  }
  // ws397: the wake key is OK (GPIO5, RTC-capable, EXT1 low). PWR cannot wake:
  // the PMIC IRQ is on GPIO38, which is not an RTC GPIO. A PWR press while
  // asleep only latches status in the AXP2101 (flushed by PowerKey::begin()
  // on the next boot); a 10 s PWR hold makes the PMIC cut the rails (PressOff,
  // the deliberate hardware escape), after which PWR held 1 s powers it back on.
  ensureSomeWakeSource();
  powerManager.startDeepSleep(gpio);
}

// ws397: el temporizador y los recordatorios tienen que sonar aunque el aparato
// esté despierto en otra pantalla. Antes solo los miraba el tick del hub y el
// arranque después de dormir, así que un temporizador vencido en Notas, Agenda o
// Ajustes no sonaba nunca. Se dispara sobre las pantallas tranquilas; el lector y
// las que usan red o audio se dejan en paz (ahí manda el wake por deep sleep).
// Ventana del doble toque de Atrás. Eran 500 ms, que es la medida de un doble
// clic de mouse — y esto no es un mouse: es un botón físico en un aparato de
// tinta que NO da ninguna señal entre un toque y el otro. El que lo prueba
// toca, no ve pasar nada, y recién ahí toca de nuevo: eso son 700 u 800 ms
// tranquilamente. Y si el primer toque cambió de pantalla (en Notas o en la
// agenda, Atrás sale), en el medio hay un cambio de Activity que toma el
// candado del render. 1,2 s es un gesto que se puede hacer a propósito y sigue
// lejos de dos Atrás separados de verdad.
constexpr unsigned long DOUBLE_BACK_MS = 1200;
// A partir de acá Atrás fue MANTENIDO, no tocado: es el gesto de sincronizar en
// el hub y el de abrir el menú del ítem en las listas. Por debajo del umbral más
// chico de esos dos (1 s), así que ningún gesto largo se cuela como toque.
constexpr unsigned long LONG_BACK_MS = 600;
// Dos sueltas a menos de esto no son dos toques: son el rebote de una. En el
// log había un "doble Atrás (104 ms)" 170 ms después de un Atrás mantenido —
// nadie toca dos veces en un décimo de segundo. Y después de un mantenido, la
// suelta puede rebotar: medio segundo de cuarentena.
constexpr unsigned long MIN_DOUBLE_BACK_MS = 150;
constexpr unsigned long AFTER_LONG_BACK_MS = 500;
// Media hora de ocio sin poder reposar ni una vez: algo lo está bloqueando y en
// "siempre encendido" nadie más va a mandar a dormir. Ver la red de seguridad
// en el loop.
constexpr unsigned long REST_BLOCKED_GIVE_UP_MS = 30UL * 60UL * 1000UL;
// REV-062: LA MÚSICA NO ES UN BLOQUEO SIN EXPLICAR, y por eso no le vale la
// media hora de arriba.
//
// Esa red existe para el caso "algo dejó el reposo trabado y nadie va a
// destrabarlo" — una Activity que pide `preventAutoSleep()` para siempre, un
// WiFi que quedó arriba. La música es lo contrario: la puso una persona a
// propósito y suena porque tiene que sonar. Con el plazo común, un disco
// entero sin tocar el aparato se cortaba a los ~30 min 30 s, justo lo que la
// política de unas líneas más arriba dice que NO tiene que pasar (la música
// reinicia `lastActivityTime` para eso).
//
// Pero tampoco es infinito: "la música con repetir no se corta nunca" es un
// problema que el dueño ya tenía anotado, y una lista en repetir olvidada se
// come la noche. Tres horas es más que cualquier disco o lista razonable y
// bastante menos que una batería. Es una decisión de producto, no técnica: si
// el dueño la quiere más corta o más larga, se cambia acá.
constexpr unsigned long MUSIC_GIVE_UP_MS = 3UL * 60UL * 60UL * 1000UL;
// El motivo de la música, como literal con nombre: la red de seguridad lo
// compara POR PUNTERO para saber si lo único que bloquea es eso.
constexpr const char* RAZON_MUSICA = "está sonando la música";
// Una alarma que venció hace más de esto es basura de una sesión vieja, no algo
// que el usuario esté esperando: suena sola en cualquier pantalla y no hay forma
// de entender por qué.
constexpr long STALE_ALARM_S = 2 * 60 * 60;

// Pantallas "tranquilas" (ver abajo) pero con el micrófono abierto NO lo son:
// Notas, Agenda y Calendario ahora graban, y un recordatorio que se abriera
// encima dejaba el micrófono colgado y la toma perdida.
static bool busyRecording() { return VoiceRecorder::anyRecording(); }

static bool isCalmScreen(const char* name) {
  // Pantallas tranquilas: ahi suenan los recordatorios y el temporizador, y
  // anda el doble Atras para hablar. Calendar es una lista quieta igual que
  // Agenda, asi que entra (si no, en el calendario no sonaria una alarma).
  static const char* CALM[] = {"Hub", "Home", "Agenda", "Notes", "Settings", "Weather", "Calendar", "Music"};
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
// ESTO SE DIAGNOSTICA DESDE EL LOG O NO SE DIAGNOSTICA. El atajo o abre Hablar
// o no hace nada, y "no hace nada" tiene cuatro causas distintas que desde el
// vidrio son idénticas: los toques llegaron demasiado separados, la pantalla no
// está en la lista de las tranquilas, hay una grabación abierta, o el segundo
// toque no se vio. Antes no se anotaba ninguna: sólo salía una línea cuando
// funcionaba, que es justo cuando no hace falta. Ahora cada toque deja su
// renglón con el número, así el aparato dice cuál de las cuatro es.
// Woke::Button la pone; la pasada siguiente del loop la consume (ver arriba
// de checkVoiceShortcut y el update() de la entrada).
static bool restWakeHeldPending = false;

static void checkVoiceShortcut() {
  static unsigned long lastBackRelease = 0;
  static unsigned long backPressedAt = 0;
  static unsigned long lastLongBackRelease = 0;
  // Dónde estaba la pantalla cuando se APRETÓ: esto corre antes del loop de la
  // Activity, y Hablar sale con el flanco de bajada, así que al soltar ya está
  // el hub en frente y no se sabría de dónde vino el toque.
  static bool pressedOnExitScreen = false;
  if (mappedInputManager.wasPressed(MappedInputManager::Button::Back)) {
    backPressedAt = millis();
    pressedOnExitScreen = !isCalmScreen(activityManager.currentActivityName());
  }
  if (!mappedInputManager.wasReleased(MappedInputManager::Button::Back)) return;
  const unsigned long now = millis();
  // EL ATRÁS QUE CIERRA UNA PANTALLA NO ES EL PRIMER TOQUE. Cancelar Hablar
  // con Atrás abría la ventana del atajo, y el toque siguiente —"¿por qué no
  // volvió al hub?", toco de nuevo— abría Hablar otra vez: un bucle en el que
  // el botón parecía hacer cualquier cosa. Vale para toda pantalla que usa
  // Atrás para salir (Hablar, Noticias, la Biblia, una app de Lua): ese toque
  // ya hizo lo suyo.
  if (pressedOnExitScreen) {
    pressedOnExitScreen = false;
    backPressedAt = 0;
    lastBackRelease = 0;
    LOG_DBG("MAIN", "Atrás cerró una pantalla: no abre la ventana del atajo de voz");
    return;
  }
  // UNA PULSACIÓN LARGA NO ES UN TOQUE. `wasLongPressed()` marca la suelta como
  // suprimida, pero `wasReleased()` NO mira esa marca (sólo la mira
  // `consumeSuppressedRelease()`, que usa el camino del botón de despertar), así
  // que mantener Atrás —que en el hub sincroniza y en las listas abre el menú
  // del ítem— llegaba acá como un toque igual: sincronizar y después tocar una
  // sola vez abría Hablar sin que nadie lo pidiera.
  const unsigned long held = backPressedAt != 0 ? now - backPressedAt : 0;
  backPressedAt = 0;
  if (held > LONG_BACK_MS) {
    lastBackRelease = 0;
    lastLongBackRelease = now;
    LOG_DBG("MAIN", "Atrás mantenido %lu ms: no cuenta para el atajo de voz", held);
    return;
  }
  if (lastLongBackRelease != 0 && now - lastLongBackRelease < AFTER_LONG_BACK_MS) {
    LOG_DBG("MAIN", "Atrás %lu ms después de un Atrás mantenido: rebote, no cuenta", now - lastLongBackRelease);
    return;
  }
  const unsigned long gap = lastBackRelease != 0 ? now - lastBackRelease : 0;
  if (lastBackRelease != 0 && gap < MIN_DOUBLE_BACK_MS) {
    LOG_DBG("MAIN", "Atrás a %lu ms del anterior: rebote, no cuenta", gap);
    return;
  }
  const bool isDouble = lastBackRelease != 0 && gap <= DOUBLE_BACK_MS;
  lastBackRelease = isDouble ? 0 : now;  // el segundo toque cierra la ventana
  if (!isDouble) {
    if (gap > 0) {
      LOG_INF("MAIN", "Atrás: %lu ms desde el anterior, fuera de la ventana de %lu", gap, DOUBLE_BACK_MS);
    } else {
      LOG_DBG("MAIN", "Atrás: primer toque, se abre la ventana del atajo de voz");
    }
    return;
  }
  const char* name = activityManager.currentActivityName();
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) {
    LOG_INF("MAIN", "doble Atrás (%lu ms) en %s: el lector no lo usa", gap, name);
    return;
  }
  if (busyRecording()) {
    LOG_INF("MAIN", "doble Atrás (%lu ms): ya hay una grabación abierta", gap);
    return;
  }
  if (!isCalmScreen(name)) {
    // A propósito: acá Atrás es el botón con el que cada app sale, y robárselo
    // dejaría pantallas de las que no se puede salir. Pero que se sepa.
    LOG_INF("MAIN", "doble Atrás (%lu ms) en %s: esa pantalla usa Atrás para salir", gap, name);
    return;
  }
  LOG_INF("MAIN", "doble Atrás (%lu ms) desde %s: se abre Hablar", gap, name);
  activityManager.pushActivity(makeUniqueNoThrow<VoiceActivity>(renderer, mappedInputManager));
}

// La alarma NO usa la lista blanca de pantallas tranquilas. Esa lista existe
// para no robarle los controles a cada app (doble Atras, gestos), pero aplicada
// a las alarmas dejaba mudos los doce juegos, las apps en Lua, Noticias, Fotos,
// la Biblia y el Traductor: el temporizador vencia y no sonaba nunca. Ahora se
// pregunta al reves: suena en todos lados MENOS donde molestaria de verdad.
static bool isAlarmSilentScreen(const char* name) {
  // Timer y ReminderAlert SON la alarma (apilar otra encima seria un bucle);
  // Sleep es la barrita de apagado y Boot/Crash son pantallas de sistema.
  static const char* SILENT[] = {"Timer", "ReminderAlert", "Sleep", "Boot", "Crash"};
  for (const char* n : SILENT) {
    if (strcmp(name, n) == 0) return true;
  }
  return false;
}

// ¿Una alarma vencida sonaría AHORA, en la pantalla de turno? Son las mismas
// preguntas con las que abre `checkTimeAlarms()`, sacadas aparte porque el
// reposo necesita la respuesta: si acá no va a sonar, tener algo vencido no
// tiene por qué impedir reposar (ver el tope en el loop).
static bool alarmWouldRingHere() {
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) return false;
  if (busyRecording()) return false;
  if (activityManager.preventAutoSleep()) return false;
  return !isAlarmSilentScreen(activityManager.currentActivityName());
}

// Devuelve true cuando puso una alarma en pantalla: el llamador tiene que
// tratar eso como actividad, o el reposo se lo lleva puesto antes de pintarlo.
static bool checkTimeAlarms() {
  // Las cuatro guardias viven en `alarmWouldRingHere()`, que también consulta el
  // reposo: si estuvieran escritas dos veces, se separarían (ya pasó con las
  // rutas protegidas en 1.5.91) y el reposo decidiría con un criterio distinto
  // del que de verdad hace sonar la alarma. `preventAutoSleep()` es "esta
  // pantalla está ocupada AHORA": red arriba, hablando, descargando.
  if (!alarmWouldRingHere()) return false;
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) return false;
  if (HUB_STORE.timerRunning() && HUB_STORE.timerEndAt <= now) {
    // Un temporizador que venció hace horas no tiene por qué sonar ahora: eso
    // pasa cuando el aparato estuvo apagado o sin reloj y deja una alarma
    // fantasma que salta sola en cualquier pantalla. Se descarta en silencio.
    if (now - HUB_STORE.timerEndAt > STALE_ALARM_S) {
      LOG_INF("MAIN", "temporizador vencido hace %ld s: se descarta", static_cast<long>(now - HUB_STORE.timerEndAt));
      HUB_STORE.clearTimer();
      HUB_STORE.saveToFile();
      return false;
    }
    LOG_INF("MAIN", "suena el temporizador desde %s", activityManager.currentActivityName());
    // Si no hay heap, `makeUniqueNoThrow` devuelve nullptr y `pushActivity` no
    // hace nada: decir que sí igual deja al llamador contándolo como actividad
    // y el aparato reintentando cada cinco segundos sin dormir nunca.
    auto timer = makeUniqueNoThrow<TimerActivity>(renderer, mappedInputManager, 0, /*resumeFired=*/true);
    if (!timer) return false;
    activityManager.pushActivity(std::move(timer));
    return true;
  }
  if (const HubStore::Reminder* due = HUB_STORE.dueReminder(now)) {
    LOG_INF("MAIN", "suena el recordatorio %d desde %s", due->id, activityManager.currentActivityName());
    auto alerta =
        makeUniqueNoThrow<ReminderAlertActivity>(renderer, mappedInputManager, due->id, due->title, due->when);
    if (!alerta) return false;
    activityManager.pushActivity(std::move(alerta));
    return true;
  }
  return false;
}

// Cuánto falta para lo próximo que tiene que sonar (recordatorio o fin del
// temporizador), en ms, o 0 si no hay nada. El reposo lo usa como tope del
// ciclo: dormir 2 s de más no se nota, dormir 10 minutos de más sí.
static unsigned long msUntilNextAlarm() {
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) return 0;
  // El mismo criterio que el deep sleep: lo vencido primero. Si no, reposando
  // se repetía el mismo error, con el agravante de que el ciclo de reposo puede
  // durar horas cuando los gestos están apagados.
  const time_t due = nextWakeInstant(now);
  // La alarma del chip es la que aguanta las esperas largas: el timer del light
  // sleep se corta a la hora (más allá de eso no vale la pena estar
  // recontando), pero el PCF85063 despierta por GPIO45 en el segundo exacto
  // aunque el aparato lleve ocho horas reposando.
  bool rtcDespierta = false;
  if (due > now) {
    // Que la alarma esté ARMADA no alcanza: para que despierte, el INT del chip
    // (GPIO45) tiene que estar usable. Si no lo está y devolvemos 0, el reposo
    // queda sin timer y sin fuente de despertar, y el recordatorio no suena
    // nunca. Las dos condiciones se preguntan juntas.
    rtcDespierta = RTC_ALARM.armAt(due, now) && IDLE_SLEEP.rtcIntUsable();
  } else if (RTC_ALARM.armedAt() != 0) {
    RTC_ALARM.disarm();
  }
  if (due == 0) return 0;
  if (due <= now) return 1;
  const time_t seconds = due - now;
  // Más de una hora: si el chip del RTC puede despertarnos, no hace falta timer
  // y el reposo puede durar toda la noche. Si NO puede, el timer del ESP se
  // corta a la hora y se vuelve a recontar: cuesta un despertar por hora, que
  // es infinitamente menos que perder el recordatorio.
  if (seconds > 3600) return rtcDespierta ? 0UL : 3600UL * 1000UL;
  return static_cast<unsigned long>(seconds) * 1000UL;
}

// ws397: los gestos del IMU valen desde cualquier pantalla tranquila, igual que
// el doble Atrás. Son tres, y los tres resuelven algo que con tres botones sale
// incómodo:
//
//   boca abajo ...... callar lo que esté sonando (recordatorio, temporizador,
//                     música). Es el gesto de "ahora no" de toda la vida.
//   sacudir ......... cancelar: corta una grabación en curso o descarta la
//                     alarma que quedó en pantalla.
//   doble golpe ..... abrir Hablar, el mismo PTT del doble Atrás, sin buscar
//                     ningún botón.
//
// El resto de los eventos (inclinar, girar) son de cada pantalla: acá se
// consumen SOLO los tres globales, así el laberinto se queda con los suyos.
static void checkMotionGestures() {
  if (!MOTION.available()) return;
  const MotionInput::Event pending = MOTION.pending();
  if (pending == MotionInput::Event::None) return;

  // La pantalla de diagnóstico se queda con TODOS los eventos: es la que los
  // cuenta. Si acá se consume "boca abajo" para pausar la música, el gesto no
  // llega nunca al contador y la pantalla parece rota.
  if (strcmp(activityManager.currentActivityName(), "Motion") == 0) return;

  // Sacudir mientras se graba corta la toma, esté donde esté el usuario: es lo
  // único que se atiende con una grabación abierta.
  if (pending == MotionInput::Event::Shake && busyRecording()) {
    MOTION.take(MotionInput::Event::Shake);
    LOG_INF("MAIN", "sacudida: se cancela la grabación");
    VoiceRecorder::abortAll();
    return;
  }

  const char* name = activityManager.currentActivityName();
  // El temporizador y el recordatorio que suenan se callan solos: el gesto se
  // les deja pasar sin consumir, porque ahí "boca abajo" además pospone, y eso
  // solo lo sabe hacer la Activity.
  if (strcmp(name, "Timer") == 0 || strcmp(name, "ReminderAlert") == 0) return;

  if (pending == MotionInput::Event::FaceDown && MUSIC.isActive() && !MUSIC.isPaused()) {
    MOTION.take(MotionInput::Event::FaceDown);
    LOG_INF("MAIN", "boca abajo: se pausa la música");
    MUSIC.togglePause();
    return;
  }

  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) return;
  if (busyRecording()) return;
  // El DOBLE GOLPE no usa ningún botón, así que no le roba los controles a
  // nadie: anda en cualquier pantalla que no esté ocupada, igual que las
  // alarmas. Con la lista blanca de nueve pantallas el gesto no existía en los
  // juegos, en Noticias, en Fotos ni en la Biblia, que es justo donde uno lo
  // prueba y concluye que los gestos no están hechos. El doble Atrás SÍ sigue
  // atado a la lista: ahí Atrás es un botón que cada app usa para salir.
  if (activityManager.preventAutoSleep()) return;
  if (isAlarmSilentScreen(name)) return;

  if (pending == MotionInput::Event::DoubleTap) {
    MOTION.take(MotionInput::Event::DoubleTap);
    LOG_INF("MAIN", "doble golpe: se abre Hablar desde %s", name);
    activityManager.pushActivity(makeUniqueNoThrow<VoiceActivity>(renderer, mappedInputManager));
  }
}

// ws397: el fondo de pantalla es la pantalla de información, y es lo ÚLTIMO
// que se pinta antes de que el sistema se muera. Va con la tarjeta todavía
// montada (más adelante `Storage.prepareForDeepSleep()` la desmonta y
// `display.deepSleep()` apaga el panel, así que este es el último momento
// posible) y los titulares se leen ACÁ, antes de que la SD se vaya.
//
// Reemplaza a las dos pinturas que se pagaban antes: la pantalla de sueño del
// SDK y, encima, la foto elegida en Ajustes → Fondo de pantalla. Las fotos
// salieron del producto.
static void paintWallpaperForSleep(const sleepscreen::State state = sleepscreen::State::Suspended) {
  const std::vector<std::string> news = sleepscreen::readHeadlines();
  if (!sleepscreen::paint(renderer, state, news)) {
    LOG_ERR("MAIN", "fondo de pantalla: no se pudo pintar");
  }
}

// APAGAR, no dormir. Deja el fondo de pantalla puesto (el panel es biestable:
// lo que queda pintado se queda pintado con el aparato muerto), desmonta la
// tarjeta y le pide al PMIC que corte los rieles. Vuelve sólo si el PMIC no
// contestó, para que el llamador se conforme con dormir.
static void powerOffNow() {
  loopwdt::pause("apagado");
  MUSIC.stop();
  HUB_STORE.saveToFile();
  APP_STATE.showBootScreen = false;
  APP_STATE.saveToFile();
  paintWallpaperForSleep(sleepscreen::State::PoweredOff);
  // NO se corta con el boton todavia apretado. El fondo tarda ~2 s en pintarse
  // y el usuario sigue con el dedo puesto; si el PMIC corta los rieles con
  // PWRON abajo, lo vuelve a encender enseguida (PressOn), el firmware arranca
  // con la tecla mantenida, la toma como pulsacion nueva, a los 3 s vuelve a
  // apagar... y asi hasta sacar la bateria. Se espera la suelta (el corte duro
  // del propio PMIC a los 10 s sigue siendo el escape).
  const unsigned long waitFrom = millis();
  while (POWER_KEY.pressed() && millis() - waitFrom < 8000) {
    POWER_KEY.pump();
    delay(20);
  }
  LOG_INF("MAIN", "PWR soltado tras %lu ms de espera: se apaga", millis() - waitFrom);
  devlog::event("MAIN", "apagado por PWR mantenido");
  devlog::close();
  Storage.prepareForDeepSleep();
  if (!POWER_KEY.powerOff()) {
    LOG_ERR("MAIN", "el PMIC no aceptó el apagado: se duerme");
    return;
  }
  // El corte no es instantaneo: el PMIC baja los rieles en unos ms. Si en dos
  // segundos seguimos vivos, no corto: se cae al deep sleep del llamador.
  delay(2000);
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  loopwdt::pause("preparando el sueño");

  // REV-073: EL DESPERTADOR SE DECIDE ACÁ, ANTES DE TOCAR NADA.
  //
  // Estaba diez líneas más abajo, después de pintar el fondo, apagar el WiFi y
  // dormir el panel — o sea después de que todo fuera irreversible. Si el timer
  // no se podía armar, lo único que pasaba era una línea de log:
  // `ensureSomeWakeSource()` veía que OK puede despertar, se daba por
  // satisfecho, y el aparato se dormía con un recordatorio pendiente que no iba
  // a sonar nunca. El usuario podía despertarlo; la alarma se había perdido.
  //
  // Con la decisión acá arriba todavía se puede NO dormir, que es la única
  // respuesta correcta mientras haya un vencimiento sin despertador.
  //
  // Y no se reinicia, que sería la otra salida obvia: en este aparato PWR
  // **nunca** reinicia (esa regla costó de 1.5.96 a 1.5.99 y está escrita en
  // piedra), y este camino es justamente el de PWR mantenido. Así que se aborta
  // el sueño y se sigue despierto, que deja el aparato respondiendo y la alarma
  // viva — `checkTimeAlarms()` la va a hacer sonar desde el loop.
  //
  // Acotado, porque negarse a suspender para siempre es peor que perder una
  // alarma con su línea en el log: a la cuarta se duerme igual y se dice.
  armReminderWake();
  if (reminderWakeMissed) {
    if (reminderSleepAborts < MAX_SLEEP_ABORTS) {
      ++reminderSleepAborts;
      LOG_ERR("MAIN",
              "!!! hay una alarma pendiente y NO se pudo armar el despertador: NO se suspende (%lu de %lu). "
              "El aparato queda despierto para que la alarma suene; volvé a intentarlo en un rato",
              static_cast<unsigned long>(reminderSleepAborts), static_cast<unsigned long>(MAX_SLEEP_ABORTS));
      // El latch del armado se suelta: el próximo intento tiene que ser uno
      // nuevo de verdad. Sin esto, `armReminderWake()` volvería con el
      // `if (reminderWakeArmed) return;` del principio y ni siquiera miraría un
      // recordatorio creado mientras tanto.
      reminderWakeArmed = false;
      reminderRetries = 0;
      loopwdt::resume();
      return;
    }
    LOG_ERR("MAIN",
            "!!! OJO: tras %lu intentos sigue sin poder armarse el despertador. Se suspende igual y la "
            "alarma pendiente NO va a sonar; se despierta con OK",
            static_cast<unsigned long>(MAX_SLEEP_ABORTS));
    reminderSleepAborts = 0;
  }

  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  // REV-064: LA MÚSICA SE CORTA ACÁ, no en `sleepNow()`.
  //
  // `sleepNow()` empieza con `MUSIC.stop()`, pero corre DESPUÉS de
  // `Storage.prepareForDeepSleep()`, que desmonta el volumen. Y la música vive
  // fuera de la Activity: al suspender a mano puede seguir sonando. Su tarea
  // (`audio_play`, propia, en el otro núcleo) lee el MP3 de la tarjeta por
  // `HalFile::read()`, así que entre el desmontaje y el `MUSIC.stop()` de
  // `sleepNow()` hay una ventana en la que esa tarea puede tomar el mutex y
  // leer contra un FsVolume ya terminado. Cortarla antes de tocar nada cierra
  // la ventana entera; el `MUSIC.stop()` de `sleepNow()` se queda igual, es
  // idempotente y cubre los caminos del `setup()` que no pasan por acá.
  MUSIC.stop();
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
  // En la ws397 no se pinta la pantalla de sueño del SDK: el fondo informativo
  // la tapa entera unas líneas más abajo, así que pintarla es pagar un refresco
  // de pantalla completa para nada. De paso, el cuadro que guarda Quick Resume
  // queda siendo la pantalla anterior, que es lo que corresponde.
  activityManager.goToSleep(fromTimeout, !BoardConfig::isWS397());

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

  // El diario de la batería, con la tarjeta todavía montada: esta es la muestra
  // que cierra el tramo despierto y abre el tramo dormido, que es el largo y el
  // que de verdad dice cuánto dura.
  batterylog::sampleNow("antes de dormir");

  halTiltSensor.deepSleep();  // idempotente: sleepNow() lo repite para los caminos que no pasan por acá
  display.deepSleep();
  // El despertador ya se armó ARRIBA DE TODO (REV-073), con la SD montada y
  // cuando todavía se podía decidir no dormir. Acá era tarde: para cuando se
  // sabía que no se había podido armar, el fondo ya estaba pintado y el panel
  // dormido, así que lo único que quedaba era loguearlo.
  LOG_DBG("MAIN", "Entering deep sleep");
  devlog::close();
  Storage.prepareForDeepSleep();

  sleepNow();
}

// ws397: the physical PWR key is the AXP2101 PWRKEY (PowerKey, src/util),
// separate from OK, which is now plain Confirm (InputStyle::DigitalButtons).
// The hold is split by time:
//   short press (< 600 ms) .... "clean screen": next paint is a FULL refresh
//   600 ms .................... the sleep bar appears
//   2.2 s ..................... second banner: about to sleep
//   3 s ....................... deep sleep (enterDeepSleep)
// The PMIC's own hard cut is programmed at 10 s (PowerKey::begin), so it can
// never race the bar. The web setting shortPwrBtn is forced to IGNORE on this
// board (setup/loop): Button::Power never fires here, so the reader/page-turn/
// footnote/force-refresh bindings are inert whatever the web says.
static bool usePowerHoldTiers() { return BoardConfig::isWS397(); }

// Hold banner, so the user sees something is happening and how far to keep
// holding. Painted OVER whatever is on screen (no clear) and at most two
// paints per gesture: every e-ink repaint costs half a second, and the panel
// rule is not to burn partial refreshes for nothing.
static void drawPowerHoldBanner(const unsigned long held, const bool aboutToSleep) {
  RenderLock lock;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int boxW = screenW - 56;
  const int x = (screenW - boxW) / 2;
  const int textW = boxW - 32;

  // El texto son dos frases separadas por " · " ("Suelta para suspender",
  // "3 s para apagar"): van en dos renglones, porque en uno solo se salían del
  // cuadro. Si un renglón igual no entra en UI_12, baja a UI_10.
  const StrId what = aboutToSleep ? StrId::STR_PWR_HOLD_SLEEPING : StrId::STR_PWR_HOLD_OFF;
  const std::string all = I18N.get(what);
  std::string lines[2];
  int nLines = 1;
  const size_t sepAt = all.find(" \xC2\xB7 ");
  if (sepAt != std::string::npos) {
    lines[0] = all.substr(0, sepAt);
    lines[1] = all.substr(sepAt + 4);
    nLines = 2;
  } else {
    lines[0] = all;
  }
  int fontId = UI_12_FONT_ID;
  for (int i = 0; i < nLines; i++) {
    if (renderer.getTextWidth(fontId, lines[i].c_str(), EpdFontFamily::BOLD) > textW) fontId = UI_10_FONT_ID;
  }
  const int lineH = renderer.getLineHeight(fontId);
  // EL CUADRO MIDE SIEMPRE LO MISMO. "Suelta para suspender · 3 s para apagar"
  // son dos renglones y "Apagando..." es uno: con el alto según el texto, el
  // segundo cuadro salía más chico y más abajo que el primero, y como el
  // framebuffer conserva lo que se pintó antes, el borde y el texto del cuadro
  // grande quedaban asomando alrededor del chico ("se superpone la ventana de
  // la barrita con la de apagando"). Se reserva el alto de dos renglones y el
  // texto de uno se centra en ese hueco.
  constexpr int TEXT_LINES = 2;
  const int textBlockH = TEXT_LINES * (lineH + 4);
  const int boxH = 24 + textBlockH + 12 + 16 + 24;
  const int y = screenH / 2 - boxH / 2;

  renderer.fillRoundedRect(x, y, boxW, boxH, 16, Color::White);
  renderer.drawRoundedRect(x, y, boxW, boxH, 3, 16, true);
  const int textTop = y + 24 + (textBlockH - nLines * (lineH + 4)) / 2;
  for (int i = 0; i < nLines; i++) {
    renderer.drawCenteredText(fontId, textTop + i * (lineH + 4),
                              renderer.truncatedText(fontId, lines[i].c_str(), textW, EpdFontFamily::BOLD).c_str(),
                              true, EpdFontFamily::BOLD);
  }

  // Bar: how much is left until sleep.
  const int barX = x + 24;
  const int barW = boxW - 48;
  const int barY = y + 24 + textBlockH + 12;
  constexpr int barH = 16;
  renderer.drawRect(barX, barY, barW, barH, 2, true);
  // La barra mide lo que falta para apagar DESDE QUE APARECE: con el 0 en cero
  // absoluto nacía al 40 % (1200 de 3000) y se apagaba al 88 %, o sea que ni
  // empezaba vacía ni terminaba llena, y por eso parecía que no cargaba.
  const unsigned long span = POWER_HOLD_OFF_MS - POWER_HOLD_ACTION_MS;
  const unsigned long done = held <= POWER_HOLD_ACTION_MS ? 0 : std::min(held - POWER_HOLD_ACTION_MS, span);
  const int filled = static_cast<int>(barW * done / span);
  if (filled > 4) renderer.fillRect(barX + 2, barY + 2, filled - 4, barH - 4, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Returns true when it kept the loop pass (banner on screen, or the action
// already fired) so the activity underneath does not repaint over it. The key
// state comes from POWER_KEY, pumped at the top of loop(): a release seen late
// (the loop was inside a refresh) is still a release, never a sleep.
static bool handlePowerHold(const bool gateOpen) {
  static int bannerStage = 0;  // 0 no banner, 1 banner, 2 warning

  const bool pressed = POWER_KEY.pressed();
  const unsigned long held = POWER_KEY.heldMs();

  // Una pulsacion que empezo antes de que el aparato terminara de arrancar es
  // la que lo ENCENDIO (o la que lo apago recien y no se solto): no cuenta.
  // powerReleasedSinceWake no alcanza para esto porque al arrancar el decoder
  // todavia no la vio (el estado latcheado del PMIC se descarta en begin()) y
  // "no apretado" se tomaba por "ya solto".
  if (pressed && POWER_KEY.pressStartMs() < BOOT_KEY_IGNORE_MS) {
    static bool said = false;
    if (!said) {
      said = true;
      LOG_INF("MAIN", "PWR mantenido desde el arranque (%lu ms): se ignora hasta soltar", held);
    }
    return false;
  }

  if (pressed) {
    // No sleep permission yet (just woke / just booted) or DOWN held (screenshot
    // combo): the hold is not ours. Se dice UNA vez por pulsación: "no carga la
    // barrita" puede ser el PMIC que no reportó el flanco (eso lo dice PWRKEY)
    // o esta guarda, y desde afuera son lo mismo.
    static unsigned long ignoredPressAt = 0;
    if (!gateOpen || gpio.isPressed(HalGPIO::BTN_DOWN)) {
      const unsigned long startAt = millis() - held;
      if (ignoredPressAt == 0 || startAt - ignoredPressAt > 100) {
        ignoredPressAt = startAt;
        LOG_INF("MAIN", "PWR apretado pero ignorado: gate=%d abajo=%d held=%lu ms", gateOpen ? 1 : 0,
                gpio.isPressed(HalGPIO::BTN_DOWN) ? 1 : 0, held);
      }
      return false;
    }
    if (held >= POWER_HOLD_OFF_MS) {
      LOG_INF("MAIN", "PWR mantenido %lu ms: se apaga", held);
      bannerStage = 0;
      powerOffNow();
      // REV-057: si el PMIC no cortó, se cae a `sleepNow()` y NO a
      // `enterDeepSleep()`.
      //
      // `powerOffNow()` ya desmontó el volumen (`Storage.prepareForDeepSleep()`)
      // antes de pedir el corte, así que volver a entrar por `enterDeepSleep()`
      // repetía trabajo de filesystem sobre un FsVolume ya terminado:
      // `APP_STATE.saveToFile()`, el cuadro de Quick Resume, la lectura de los
      // titulares para el fondo y la muestra de la batería. El contrato de
      // HalStorage dice justo lo contrario — después de `prepareForDeepSleep()`
      // no puede quedar ningún usuario del filesystem.
      //
      // Y de paso arregla algo que el hallazgo no nombra: `enterDeepSleep()`
      // vuelve a pintar el fondo de SUSPENDIDO encima del de APAGADO que
      // `powerOffNow()` acaba de dejar en el vidrio. El usuario mantuvo PWR
      // para apagar; el cartel tiene que decir lo que él pidió.
      //
      // `sleepNow()` no toca el filesystem ni pinta: apaga el IMU, el códec y
      // el amplificador, arma el despertador y corta los rieles. Todo lo demás
      // ya lo hizo `powerOffNow()`.
      deepSleepInProgress = true;
      LOG_ERR("MAIN", "el apagado no cortó: se suspende con la tarjeta ya desmontada");
      sleepNow();
      return true;
    }
    if (held >= POWER_HOLD_ACTION_MS) {
      // Un paso por cada POWER_HOLD_STEP_MS desde que aparece la barrita, así
      // se llena parejo. `bannerStage` es el número del último paso pintado.
      const int step = 1 + static_cast<int>((held - POWER_HOLD_ACTION_MS) / POWER_HOLD_STEP_MS);
      if (step > bannerStage) {
        if (bannerStage == 0) {
          POWER_KEY.consumeHold();  // the release after the bar is not a short press
          LOG_INF("MAIN", "PWR mantenido %lu ms: barrita", held);
        }
        bannerStage = step;
        drawPowerHoldBanner(held, held >= POWER_HOLD_WARN_MS);
      }
    }
    return bannerStage != 0;
  }

  // SOLTÓ. La regla del dueño, escrita en 1.5.96 y que hasta acá no se cumplía:
  // APRETAR Y SOLTAR = SUSPENDER, dure lo que dure la pulsación antes de los 3 s
  // de apagado. La barrita no es un umbral: sólo dice cuánto falta para apagar,
  // y soltar con ella a medias suspende igual. Hasta 1.5.98 un toque de menos
  // de 1,2 s "no hacía nada" —una regla de 1.5.59, de cuando el toque corto
  // abría un menú que ya no existe— y el dueño lo veía como que el botón no
  // andaba.
  const bool released = POWER_KEY.tookRelease();
  POWER_KEY.tookShortPress();  // latch viejo: se vacía para que no quede colgado
  const bool hadBanner = bannerStage != 0;
  bannerStage = 0;
  if (!released) return false;
  const unsigned long lastHold = held;  // heldMs() guarda el largo del hold que terminó
  if (POWER_KEY.pressStartMs() < BOOT_KEY_IGNORE_MS) {
    LOG_INF("MAIN", "PWR soltado a los %lu ms: era la pulsación que lo encendió, se ignora", lastHold);
    if (hadBanner) activityManager.requestUpdate();
    return false;
  }
  if (!gateOpen || gpio.isPressed(HalGPIO::BTN_DOWN)) {
    LOG_INF("MAIN", "PWR soltado a los %lu ms pero ignorado: gate=%d abajo=%d", lastHold, gateOpen ? 1 : 0,
            gpio.isPressed(HalGPIO::BTN_DOWN) ? 1 : 0);
    if (hadBanner) activityManager.requestUpdate();
    return false;
  }
  LOG_INF("MAIN", "PWR soltado a los %lu ms: se suspende", lastHold);
  enterDeepSleep();
  return true;  // no se llega: enterDeepSleep termina en esp_deep_sleep_start
}

// El MENÚ DE PANTALLA se sacó: PWR hace una sola cosa, mantenerlo. Con él se
// fueron "limpiar" y "bloquear", que eran sus otras dos entradas — limpiar ya lo
// hace solo el coordinador de refresco (un completo cada 12 parciales) y el
// bloqueo se quedó sin puerta; si hace falta, entra en Ajustes → Sistema.

// EL PANEL NO CONTESTA (1.5.108). El dueño lo describió como "se trabó por
// completo, no respondía ni a la palanca, a los años se conectó a la red": el
// log de ese aparato tenía un `refresh FULL 30086ms` y, después, un init de la
// pantalla de 90 s y cada refresco de 30 s. No estaba colgado: BUSY del
// SSD1677 se quedó en alto y cada comando esperaba el tope del SDK entero.
// Sacarle la batería no lo arregló porque el USB estaba puesto y el PMIC —que
// no se resetea con el ESP— siguió alimentando el panel: un controlador
// trabado no sale de ahí sin un corte de corriente DE VERDAD. Eso es lo que
// hace esto, una sola vez por encendido: ALDO1-3 abajo medio segundo, arriba,
// y reinicio limpio. Si al volver sigue mudo, se arranca igual con el tope de
// 5 s por pintada (lento, pero se llega a Ajustes y a la OTA) y el log dice
// qué pasa en vez de callarse.
void checkPanelAfterInit(unsigned long initMs) {
  const uint32_t timeouts = display.busyTimeouts();
  if (timeouts == 0) {
    if (rescue::attempted(panelRescueMagic)) {
      LOG_INF("MAIN", "el panel volvió a contestar después del ciclo de corriente (init %lu ms)", initMs);
      panelRescueMagic = 0;
    }
    return;
  }
  LOG_ERR("MAIN", "EL PANEL NO CONTESTA: BUSY quedó en alto, %lu esperas vencidas en el init (%lu ms)",
          static_cast<unsigned long>(timeouts), initMs);
  if (rescue::done(panelRescueMagic)) {
    LOG_ERR("MAIN",
            "ya se le dio un ciclo de corriente y sigue mudo: se arranca igual (cada pintada tarda el tope). "
            "Probar PWR 10 s o sacar batería Y cable; si persiste, es el panel o su cable plano");
    return;
  }
  // El PMIC no dejó cortar los rieles las veces anteriores, así que el rescate
  // NO se hizo nunca. Se reintenta, pero con tope: cada intento termina en un
  // reinicio y sin tope esto sería un bucle de arranques.
  if (!rescue::mayCycle(panelRescueMagic)) {
    LOG_ERR("MAIN",
            "el PMIC no dejó cortar los rieles en %lu intentos: el ciclo de corriente NUNCA llegó a hacerse. "
            "Se arranca igual (cada pintada tarda el tope). Mirar la línea «AXP2101 rieles:»; si el riel está "
            "bien, probar PWR 10 s o sacar batería Y cable",
            static_cast<unsigned long>(rescue::MAX_TRIES));
    return;
  }
  const uint32_t intento = rescue::failedTries(panelRescueMagic) + 1;
  LOG_ERR("MAIN", "ciclo de corriente al panel y reinicio (intento %lu de %lu)", static_cast<unsigned long>(intento),
          static_cast<unsigned long>(rescue::MAX_TRIES));
  panelRescueMagic = rescue::markAttempt(panelRescueMagic);
  const bool cycled = POWER_KEY.railsCycle(500);
  panelRescueMagic = rescue::afterCycle(panelRescueMagic, cycled);
  if (!cycled) {
    LOG_ERR("MAIN", "el PMIC no dejó cortar los rieles: se reinicia igual y se vuelve a probar al arrancar");
  }
  devlog::flush();
  delay(50);
  ESP.restart();
}

// Vigilancia en el loop: una espera de BUSY vencida en uso deja su línea en el
// acto (antes, un refresco de 30 s se veía como un `refresh … 30086ms` de nivel
// DBG y nada más). A la tercera de la sesión —un panel que no volvió solo— se
// hace lo mismo que en el arranque: ciclo de corriente y reinicio, una vez.
void checkPanelHealth() {
  static uint32_t seen = 0;
  const uint32_t now = display.busyTimeouts();
  if (now == seen) return;
  seen = now;
  LOG_ERR("MAIN", "EL PANEL NO CONTESTÓ: la espera de BUSY venció (van %lu en esta sesión); cada pintada tarda el tope",
          static_cast<unsigned long>(now));
  if (now < 3 || deepSleepInProgress) return;
  if (rescue::done(panelRescueMagic)) {
    LOG_ERR("MAIN", "el ciclo de corriente ya se hizo una vez en este encendido: no se repite");
    return;
  }
  if (!rescue::mayCycle(panelRescueMagic)) {
    LOG_ERR("MAIN", "el PMIC no dejó cortar los rieles en %lu intentos: no se insiste (mirar «AXP2101 rieles:»)",
            static_cast<unsigned long>(rescue::MAX_TRIES));
    return;
  }
  LOG_ERR("MAIN", "tres esperas vencidas: ciclo de corriente al panel y reinicio (intento %lu de %lu)",
          static_cast<unsigned long>(rescue::failedTries(panelRescueMagic) + 1),
          static_cast<unsigned long>(rescue::MAX_TRIES));
  panelRescueMagic = rescue::markAttempt(panelRescueMagic);
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  const bool cycled = POWER_KEY.railsCycle(500);
  panelRescueMagic = rescue::afterCycle(panelRescueMagic, cycled);
  if (!cycled) {
    LOG_ERR("MAIN", "el PMIC no dejó cortar los rieles: se reinicia igual y se vuelve a probar al arrancar");
  }
  devlog::flush();
  delay(50);
  ESP.restart();
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

  const unsigned long displayBeginAt = millis();
  display.begin(seamless);
  if (BoardConfig::isWS397()) checkPanelAfterInit(millis() - displayBeginAt);
  renderer.begin();
  activityManager.begin();
  // Con un micrófono abierto el panel no puede promover un refresco a limpieza:
  // son cientos de ms de SPI contra el DMA de RX de 90 ms. La grabadora avisa
  // acá cuando abre y cuando cierra, y vale para TODA pantalla que grabe.
  VoiceRecorder::setCleanHoldHook([](const bool hold) { renderer.holdCleanRefreshes(hold); });
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
  renderer.insertFont(UI_14_FONT_ID, ui14FontFamily);
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

  // ANTES de HalSystem::begin(), que en todo arranque que no sea un pánico
  // capturado borra el anillo de las últimas 16 líneas — justo el brownout y el
  // watchdog sin marker, que son los dos que no dejan crash_report y los únicos
  // para los que "se reinició solo" era toda la evidencia (REV-081).
  devlog::snapshotPreviousCrash(loopwdt::trippedLastBoot());
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
  if (BoardConfig::isWS397()) {
    // El enable del amplificador quedó RETENIDO en bajo durante el sueño
    // profundo (sleepNow); si no se suelta, el audio escribe el pin y el pad no
    // se entera: el parlante queda mudo hasta el próximo corte de energía.
    // Sobre un pad que no está retenido es un no-op.
    const int8_t amp = BoardConfig::ACTIVE.audio.ampEnable;
    if (amp >= 0) gpio_hold_dis(static_cast<gpio_num_t>(amp));
  }
  // ws397: PMIC power key. Configures 0x27/0x10/0x22 and the interrupt enables
  // and flushes whatever the key latched while we slept (the PMIC does not
  // reset with the ESP), so the first pump() never sees a phantom press.
  POWER_KEY.begin();

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
  // Los gestos comparten el mismo integrado que el giro para pasar página, así
  // que van después y sobre la misma instancia (ver HalTiltSensor::imu()).
  // El enable del amplificador (GPIO39) queda en bajo desde el arranque. Sin
  // esto flotaba desde el reset hasta el primer sonido, porque AudioManager
  // configura ese pin recién en begin(), que es perezoso: lo que haga un enable
  // de clase D al aire lo decide la fuga de la placa, no nosotros.
  AudioManager::silenceAmp();

  MOTION.begin();
  halClock.begin();
  // El reloj del sistema arranca en 1970 y nadie se lo ponía en hora: se
  // mantenía sólo el RTC. No se notaba en ningún lado salvo en el único que
  // importa — la fecha de un certificado TLS se valida contra el reloj del
  // SISTEMA, y en 1970 todo certificado parece "todavía no válido".
  halClock.applyToSystemClock();
  // Después de los botones y del IMU: prueba los pines que despiertan del reposo.
  // RTC_ALARM ANTES que IDLE_SLEEP, y no al revés: begin() del RtcAlarm hace
  // disarm(), que limpia la bandera AF. Con AF puesta (una alarma que venció
  // mientras el aparato dormía) el INT del PCF85063 se queda en bajo, y
  // IdleSleep::probeRtcInt() lo leería como "no usable" para toda la sesión —
  // justo la única fuente de despertar de los reposos largos.
  RTC_ALARM.begin();
  IDLE_SLEEP.begin();
  // El loop de Arduino es la tarea que más cerca está del límite (por acá pasan
  // el TLS, el parseo de EPUB y todo lo que no tiene tarea propia): se anota
  // para poder medirle el stack desde Ajustes -> Sistema -> Memoria.
  tasks::attach(tasks::Id::Loop);

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
  // ANTES de cargar nada: un `.tmp` de una escritura cortada es el archivo bueno
  // o es basura, y en los dos casos hay que resolverlo antes de que alguien lea
  // el destino. El rescate de `readFile()` es perezoso y sólo cubre a quien
  // pase por ahí; las cachés que se leen con `openFileForRead()` directo (hub,
  // viajes, noticias) quedaban afuera.
  tempsweep::run();
  // Las carpetas que el aparato espera encontrar (libros, música, apps,
  // tipografías, diccionarios) se crean si faltan, en vez de loguear que no
  // están y dejar la función muda.
  cardlayout::ensure();
  SETTINGS.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  devlog::begin();  // from here every LOG_* line also goes to the SD
  setLogSink(&devlog::write);
  POWER_KEY.logSnapshot();  // the PMIC register dump, now that it reaches /board/log
  // Desde acá una llamada de red que se quede esperando sigue atendiendo PWR y
  // Atrás (include/NetPump.h). Va después de POWER_KEY.begin() y del log, para
  // que la línea de instalación llegue a /board/log.
  netpumphooks::begin();
  // REV-065: lo que dejó anotado el supervisor si al arranque anterior lo forzó
  // él. Va acá, con el log ya abierto y antes de que pase nada más.
  loopwdt::reportBoot();
  // Cuánto se fue durmiendo, dicho por el aparato: la última línea del diario
  // es la de "antes de dormir" y ésta es la de ahora.
  if (BoardConfig::isWS397() && esp_reset_reason() == ESP_RST_DEEPSLEEP) batterylog::reportAfterSleep();
  // REV-061: lo que no se pudo decir al dormir, porque el log ya estaba
  // cerrado. Va junto al diario de batería a propósito: son la misma pregunta
  // —qué pasó mientras dormía— y si los rieles quedaron arriba, el número de
  // "dormido X %/h" de la línea de al lado se explica solo.
  // REV-070: si NO quedó confirmado que el panel y el audio tienen alimentación,
  // esto no puede seguir como un arranque normal: `sleepNow()` los cortó para
  // dormir y sin ellos la pantalla y el sonido están muertos. Un ciclo de
  // corriente a los rieles y un reinicio es el mismo remedio que ya usa el
  // rescate del panel, y es lo único que puede destrabar un PMIC que no aceptó
  // la escritura. UNA sola vez por encendido: si al volver sigue sin
  // confirmarse, se arranca igual y el log lo dice — un bucle de reinicios es
  // peor que una pantalla muerta con diagnóstico.
  if (BoardConfig::isWS397() && !POWER_KEY.railsConfirmed()) {
    if (rescue::done(railsRescueMagic)) {
      railsRescueMagic = 0;
      LOG_ERR("MAIN",
              "!!! OJO: los rieles del panel y el audio siguen sin confirmarse despues del ciclo de "
              "corriente. Se arranca igual; si la pantalla no va, es el PMIC o el bus I2C");
    } else if (!rescue::mayCycle(railsRescueMagic)) {
      // El PMIC no dejó cortar los rieles ninguna de las veces, así que el
      // rescate NUNCA se ejecutó. No se insiste más: cada intento termina en un
      // reinicio y sin tope esto sería un bucle de arranques.
      railsRescueMagic = 0;
      LOG_ERR("MAIN",
              "!!! OJO: el PMIC no dejo cortar los rieles en %lu intentos: el ciclo de corriente NUNCA "
              "llego a hacerse. Se arranca igual; mirar la linea «AXP2101 rieles:»",
              static_cast<unsigned long>(rescue::MAX_TRIES));
    } else {
      LOG_ERR("MAIN",
              "los rieles del panel y el audio no se pudieron confirmar: ciclo de corriente y reinicio (intento %lu de "
              "%lu)",
              static_cast<unsigned long>(rescue::failedTries(railsRescueMagic) + 1),
              static_cast<unsigned long>(rescue::MAX_TRIES));
      // La marca del intento va ANTES de tocar el PMIC: si el corte se llevara
      // al ESP por delante, el arranque siguiente tiene que encontrarlo contado.
      railsRescueMagic = rescue::markAttempt(railsRescueMagic);
      devlog::close();
      delay(50);
      // REV-070: el retorno NO se tira. Un ciclo que el PMIC no aceptó no gasta
      // el único intento; si lo aceptó, se gasta y no se repite.
      railsRescueMagic = rescue::afterCycle(railsRescueMagic, POWER_KEY.railsCycle(500));
      esp_restart();
    }
  } else if (BoardConfig::isWS397() && rescue::attempted(railsRescueMagic)) {
    railsRescueMagic = 0;
    LOG_INF("MAIN", "los rieles volvieron despues del ciclo de corriente");
  }
  if (BoardConfig::isWS397() && railsStuckMagic == RAILS_STUCK_MAGIC) {
    LOG_ERR("MAIN",
            "!!! OJO: al dormir NO se pudieron cortar los rieles ALDO1-3: el panel, el codec y el "
            "amplificador pasaron el sueño alimentados. Eso son varios mA de mas");
    railsStuckMagic = 0;
  }
  // ws397: the short-press binding is meaningless here (PWR short = clean
  // screen, and OK is plain Confirm), and SLEEP would make a 10 ms wake tap
  // count as verified. Force it whatever the file (or the web) says.
  if (BoardConfig::isWS397()) {
    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
    // El arreglo de desvanecido al sol se esconde de Ajustes en esta placa
    // (las tres secuencias del panel ya se apagan solas, así que no hace nada,
    // y encendido apaga el camino asíncrono y le suma ~160 ms a cada página).
    // Esconder la fila sin forzar el valor dejaría a quien lo tuviera encendido
    // pagando ese costo para siempre y sin puerta para apagarlo.
    SETTINGS.fadingFix = 0;
  }
  SERVER_STORE.loadFromFile();
  // El aparato tiene identidad propia desde el primer arranque: si no hay token
  // guardado se genera uno al azar y se persiste. Después se vincula a una
  // cuenta desde la web con el código que muestra Ajustes -> Vincular con mi
  // cuenta; nunca hay que escribir un token de 64 caracteres con la palanca.
  SERVER_STORE.ensureToken();
  HUB_STORE.loadFromFile();
  // De qué cuenta se cree el aparato, y qué hacer si resulta que ya no lo es
  // (REV-017). ServerClient decide CUÁNDO preguntar y retiene la cola hasta
  // saberlo; lo que es del aparato —tirar la caché de la cuenta anterior— se
  // hace acá.
  SERVER_CLIENT.setAccount(HUB_STORE.account);
  SERVER_CLIENT.setAccountChangedHandler([](const char* nuevaCuenta) {
    LOG_INF("MAIN", "cuenta nueva: se suelta lo que quedaba de la anterior");
    HUB_STORE.clearAccountContent();
    HUB_STORE.account = nuevaCuenta ? nuevaCuenta : "";
    HUB_STORE.saveToFile();
  });
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
      wakeKeyReleasePending = true;
      // ws397: the OK hold that woke us is BTN_CONFIRM now. Do not let the
      // reader's wasLongPressed(Confirm) (bookmark/dictionary) or any release
      // handler act on it: absorb it until it is released.
      if (BoardConfig::isWS397()) mappedInputManager.absorbHeldButton(wakeKeyLogicalButton());
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
      clocklessRetries = 0;  // el reloj contestó: la racha se corta
      dueReminder = HUB_STORE.dueReminder(nowEpoch + 30);
      timerFired = HUB_STORE.timerEndAt > 0 && HUB_STORE.timerEndAt <= nowEpoch + 30;
    } else if (isReminderWake) {
      // Despertó por el timer y el RTC no contestó: reintentar en un minuto en
      // vez de dormir sin nada armado (quedaría mudo para siempre).
      // CON TOPE. Sin tope, un RTC mudo (pila agotada, el PCF85063 que no
      // contesta) deja el aparato arrancando cada 60 s PARA SIEMPRE: sesenta
      // arranques por hora, cada uno pagando el montaje de la tarjeta, los
      // `loadFromFile` y el arranque de los periféricos. Es el peor patrón de
      // consumo que hay y no se recupera solo. Pasado el tope se duerme sin
      // timer y espera el botón: el recordatorio se pierde, pero sin reloj ya
      // estaba perdido, y al menos queda batería para que alguien lo prenda.
      if (++clocklessRetries > MAX_CLOCKLESS_RETRIES) {
        LOG_ERR("MAIN", "timer wake without a clock %d times: giving up, only the button wakes now",
                clocklessRetries - 1);
      } else {
        LOG_ERR("MAIN", "timer wake without a clock (%d/%d): retrying in 60 s", clocklessRetries,
                MAX_CLOCKLESS_RETRIES);
        esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
        wakeTimerArmed = true;  // que la red de REV-060 no lo pise con sus 300 s
      }
      // REV-058: ESTE CAMINO YA PASA POR `sleepNow()`, y con eso deja de ser la
      // excepción que CLAUDE.md venía documentando.
      //
      // Antes se apagaban A MANO el IMU y el amplificador y se llamaba directo a
      // `startDeepSleep()`. Esa copia se quedó corta cuando `sleepNow()` creció:
      // le faltaban la suspensión del ES8311, la retención del enable del
      // amplificador y —lo que más pesa— `railsOffForSleep()`. O sea que este
      // camino dormía con ALDO1-3 ENCENDIDOS: panel, códec y amplificador
      // alimentados toda la espera. Y no es un caso raro de un minuto: con el
      // RTC mudo son cinco arranques de 60 s, y después el aparato se queda
      // dormido así hasta que alguien lo toque.
      //
      // `sleepNow()` es seguro acá: `MUSIC.stop()` está guardado por sus
      // banderas (nada suena todavía en el `setup()`), `halTiltSensor` y
      // `POWER_KEY` ya están inicializados, y `armReminderWake()` no pisa el
      // timer de 60 s porque sin reloj devuelve temprano. Duplicar una lista de
      // apagados es cómo se separó de la verdad la primera vez.
      devlog::close();
      Storage.prepareForDeepSleep();
      sleepNow();
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
    activityManager.replaceActivity(
        makeUniqueNoThrow<TimerActivity>(renderer, mappedInputManager, 0, /*resumeFired=*/true));
  } else if (dueReminder) {
    activityManager.replaceActivity(makeUniqueNoThrow<ReminderAlertActivity>(
        renderer, mappedInputManager, dueReminder->id, dueReminder->title, dueReminder->when));
  } else if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        makeUniqueNoThrow<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (SetupActivity::pending()) {
    // Aparato recién salido de la caja: los primeros pasos antes que nada.
    // Se retoma solo si un paso de red lo reinicia en silencio.
    activityManager.replaceActivity(makeUniqueNoThrow<SetupActivity>(renderer, mappedInputManager));
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

  // REV-065: de acá en adelante hay alguien mirando que el loop siga latiendo.
  // Se arma al final del setup a propósito: el arranque tiene sus propias
  // esperas largas (el init del panel puede pagar el tope de BUSY) y todavía no
  // hay nadie a quien responderle.
  loopwdt::begin();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  // Arranca una pasada: se olvida el resumen de red de la anterior y se limpia
  // la cancelación. Si alguien apretó Atrás o PWR para cortar una descarga, el
  // loop ya volvió a correr: la cancelación cumplió y no puede quedar puesta
  // para matar lo próximo que se pida.
  netpump::beginPass();

  // REV-056: qué pantalla está en frente, para que la línea de "pixeles fuera
  // de pantalla" diga QUIÉN dibujó en vez de sólo dónde. Es una copia de hasta
  // 31 bytes por pasada del loop; el camino del píxel no paga nada.
  gfxscope::setActivity(activityManager.currentActivityName());

  if (BoardConfig::isWS397()) {
    // OK is plain Confirm here (DigitalButtons): the shared confirm/power
    // toggle does not apply, and the short-press binding stays IGNORE even if
    // the web settings page writes something else at runtime.
    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
  } else {
    gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  }
  mappedInputManager.update();
  // La tecla que despertó del reposo (ver Woke::Button más abajo) recién se
  // ve acá: mantenida no es un gesto largo de la pantalla nueva. La suelta
  // sigue siendo un toque normal (pasa la página, elige).
  if (restWakeHeldPending) {
    restWakeHeldPending = false;
    static const MappedInputManager::Button KEYS[] = {MappedInputManager::Button::Back,
                                                      MappedInputManager::Button::Confirm,
                                                      MappedInputManager::Button::Up, MappedInputManager::Button::Down};
    for (const MappedInputManager::Button k : KEYS) {
      if (mappedInputManager.isPressed(k)) {
        mappedInputManager.ignoreHeldLongPress(k);
        LOG_DBG("MAIN", "la tecla que despertó del reposo sigue abajo: cuenta como toque, no como mantenida");
      }
    }
  }
  POWER_KEY.pump();    // ws397: PMIC key state for this pass (no-op elsewhere)
  MOTION.poll();       // ws397: acelerómetro cada 80 ms (no-op sin IMU o sin gestos)
  shtc3::tick();       // temperatura de adentro, en dos tiempos y sin bloquear
  batterylog::tick();  // el diario de la batería, una línea cada diez minutos
  devlog::tick();      // que lo último escrito llegue a la tarjeta antes de un cuelgue
  if (BoardConfig::isWS397()) checkPanelHealth();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, music (it streams from the SD) or normal navigation
    // while its filesystem is detached. A PWR tap is dropped, not queued.
    POWER_KEY.tookShortPress();
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
    tasks::logMemory("loop");
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
  // Con la pantalla bloqueada los botones NO cuentan como actividad: el aparato
  // en la mochila tiene que poder reposar aunque la palanca se apriete sola.
  // PWR sí cuenta siempre: es el que desbloquea.
  const bool userInput =
      (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity());
  // `isSounding()` y no `isActive()`: `isActive()` es "hay una pista cargada",
  // que sigue siendo cierto EN PAUSA. Con `isActive()` una canción pausada
  // reiniciaba el contador de ocio en cada pasada y el aparato se quedaba
  // despierto a 40 mA para siempre, que es justo lo contrario de pausar. Desde
  // F04 la pausa frena la tarea y apaga el canal TX, así que pausado no hay
  // nada que proteger.
  if (userInput || activityManager.preventAutoSleep() || MUSIC.isSounding() || POWER_KEY.pressed()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }
  // Lo ÚLTIMO que hizo una persona, sin contar lo que el firmware se impone
  // solo. `lastActivityTime` lo reinician `preventAutoSleep()` y la música, así
  // que no sirve para saber si el aparato está abandonado: para eso está éste.
  static unsigned long lastUserInputTime = millis();
  if (userInput) lastUserInputTime = millis();

  // Music and alarms run BEFORE any early return below (wake release,
  // screenshot combo, hold banner): holding PWR must not freeze the track or
  // silence a reminder. The music player lives outside the Activity; this is
  // what chains the next track when one ends, wherever the user is.
  MUSIC.pump();

  // Si la red quedó arriba por cualquier motivo (Hablar, Noticias, la Biblia,
  // el Clima, un viaje…) y la pantalla de turno ya terminó lo suyo, se
  // aprovecha para subir lo pendiente y bajar lo que cambió. La guardia de
  // "desocupada" es la que hace que esto no se meta en el medio de nada: casi
  // toda Activity de red pide preventAutoSleep() MIENTRAS trabaja.
  // Y no en Hablar (allowsBackgroundSync): ahí la radio sigue arriba con la
  // respuesta en pantalla y la pantalla ya no pide preventAutoSleep(), así que
  // esto entraba a los 3 s de quietud y bloqueaba el loop justo cuando el
  // dueño tocaba Atrás en "¿otra pregunta?" para volver al hub.
  if (BoardConfig::isWS397() && !activityManager.preventAutoSleep() && !activityManager.isReaderActivity() &&
      activityManager.allowsBackgroundSync() && !busyRecording() && !MUSIC.isSounding()) {
    devicesync::ifDue(millis() - lastActivityTime);
  }
  static unsigned long lastAlarmCheck = 0;
  if (millis() - lastAlarmCheck >= 5000) {
    lastAlarmCheck = millis();
    // La bandera AF del RTC deja la línea INT en bajo hasta que se limpie, y
    // con GPIO45 en bajo el light sleep se rechaza siempre: limpiarla no es
    // opcional, es lo que evita que el aparato gire en falso gastando de más.
    if (RTC_ALARM.fired()) {
      RTC_ALARM.clearFlag();
      RTC_ALARM.disarm();  // se re-arma sola con el próximo vencimiento
      LOG_INF("MAIN", "alarma del RTC: venció");
      // Y NO se reinicia el contador de ocio: esto no lo hizo una persona, lo
      // hizo el propio firmware. `clearFlag()` y `disarm()` no comprueban la
      // escritura, así que un bus que lee bien pero no escribe deja AF puesta y
      // `fired()` en true PARA SIEMPRE: con el reinicio acá, el contador de ocio
      // se rearmaba cada cinco segundos y el aparato no volvía a dormir nunca —
      // 40 mA hasta agotar la batería. Lo que sigue (checkTimeAlarms) sí cuenta
      // como actividad, pero sólo si de verdad puso una alarma en pantalla.
    }
    if (checkTimeAlarms()) {
      lastActivityTime = millis();
    }
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, confirm, or other short-press action. The absorbed
  // release (absorbHeldButton) is cleared here too, otherwise it would swallow
  // the next genuine release instead.
  if (wakeKeyReleasePending && !gpio.isPressed(wakeKeyIndex())) {
    wakeKeyReleasePending = false;
    mappedInputManager.consumeSuppressedRelease();
    return;
  }

  // Screenshot combo: POWER + DOWN. On the ws397 POWER is the PMIC key
  // (POWER_KEY.pressed() holds between its real edges); elsewhere it is the
  // GPIO power button. handlePowerHold() already yields to DOWN.
  const bool powerHeld = BoardConfig::isWS397() ? POWER_KEY.pressed() : gpio.isPressed(HalGPIO::BTN_POWER);
  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (powerHeld && gpio.isPressed(HalGPIO::BTN_DOWN)) {
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
    if (powerHeld) return;
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
    POWER_KEY.tookShortPress();  // the release that ends the combo is not a tap
    if (BoardConfig::isWS397() || gpio.wasReleased(HalGPIO::BTN_POWER)) return;
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

  // "Hay cable" es VBUS, no "está cargando". `isUsbConnected()` en esta placa
  // pregunta si el PMIC está cargando, y con la batería llena eso da false con
  // el cable puesto. Se pregunta lo uno O lo otro. Se calcula ACÁ ARRIBA porque
  // ahora lo miran los dos: el auto-sleep (REV-067) y el reposo de más abajo.
  const bool cablePuesto = BoardConfig::isWS397() && (POWER_KEY.vbusPresent() || gpio.isUsbConnected());

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    // REV-067: EL CABLE TAMBIÉN FRENA EL SUEÑO PROFUNDO, no sólo el reposo.
    //
    // El bloque de light sleep de más abajo ya decidía esto y lo dejaba escrito:
    // el USB CDC no sobrevive al sueño y, enchufado, ahorrar batería no
    // justifica que el aparato DESAPAREZCA de la computadora. Pero ese criterio
    // no protegía al deep sleep, que se evalúa antes — y en la ws397 el tiempo
    // está forzado a diez minutos y el cable no reinicia `lastActivityTime`.
    // O sea: enchufado y sin tocarlo, a los diez minutos se iba igual y del
    // otro lado el puerto se caía solo. Es el mismo efecto que el reposo se
    // cuida de evitar, por el otro camino.
    //
    // Esto NO toca el sueño manual: mantener PWR sigue suspendiendo con el
    // cable puesto, porque eso lo pidió una persona.
    static bool avisadoCable = false;
    if (cablePuesto) {
      if (!avisadoCable) {
        avisadoCable = true;
        LOG_INF("SLP", "se cumplió el tiempo para dormir pero el cable está puesto: no se duerme solo");
      }
    } else {
      avisadoCable = false;  // si vuelve a pasar con el cable, se dice de nuevo
      LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
      enterDeepSleep(true);
      // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
      return;
    }
  }

  // ws397: la etapa del medio. Si no pasa nada pero todavía falta para el deep
  // sleep, el aparato reposa en light sleep: la pantalla queda como está (el
  // panel es biestable) y vuelve en menos de 10 ms con todo el estado intacto.
  // Va DESPUÉS del deep sleep a propósito: el deep sleep tiene precedencia, y
  // así el reposo nunca puede impedirlo.
  if (BoardConfig::isWS397()) {
    // Nada de reposar con el I2S abierto, la red arriba, la tarjeta prestada o
    // una pantalla que se pinta sola: ahí el light sleep corta lo que está en
    // curso. El USB enchufado también lo bloquea (el CDC no sobrevive, y
    // enchufado la batería no es el problema).
    // "Hay cable" es VBUS, no "está cargando". `isUsbConnected()` en esta placa
    // pregunta si el PMIC está cargando, y con la batería llena eso da false
    // con el cable puesto: el aparato reposaba enchufado, el USB CDC se caía y
    // del lado de la compu se veía como que se desconecta y se reconecta cada
    // tanto. Se pregunta lo uno O lo otro: si el registro de VBUS no contesta,
    // queda el criterio de antes.
    // (se calcula arriba del auto-sleep: lo miran los dos — REV-067)
    // Queda en el log para poder confirmarlo sin cable: si VBUS dice una cosa y
    // "está cargando" otra, es justamente el caso que rompía el reposo.
    static int lastCableState = -1;
    const int cableState = cablePuesto ? 1 : 0;
    if (cableState != lastCableState) {
      lastCableState = cableState;
      LOG_INF("MAIN", "cable %s (vbus=%d cargando=%d)", cablePuesto ? "puesto" : "sacado",
              POWER_KEY.vbusPresent() ? 1 : 0, gpio.isUsbConnected() ? 1 : 0);
    }
    // EL REPOSO TIENE QUE DECIR POR QUÉ NO ENTRA. De las razones por las que no
    // reposa, una sola dejaba rastro (el rechazo del kernel): el bloqueo y el
    // tope diminuto no decían nada, así que "el reposo nunca entró" era un
    // síntoma sin ninguna línea en el log detrás. Se anota el motivo, y sólo
    // cuando CAMBIA: es una condición que se evalúa cien veces por segundo.
    const char* porQue = nullptr;
    if (activityManager.preventAutoSleep())
      porQue = "la pantalla está ocupada";
    else if (activityManager.skipLoopDelay())
      porQue = "la pantalla corre sin pausa";
    else if (MUSIC.isSounding())
      porQue = RAZON_MUSICA;
    else if (busyRecording())
      porQue = "el micrófono está abierto";
    else if (POWER_KEY.pressed())
      porQue = "PWR está apretado";
    else if (cablePuesto)
      porQue = "el cable está puesto";
    else if (WiFi.getMode() != WIFI_MODE_NULL)
      porQue = "el WiFi está arriba";
    else if (gfxPanelRefreshInFlight())
      // EL CANDADO QUE FALTABA, y el que explica el aparato pintando dos veces.
      // Durante el light sleep las interrupciones por flanco no se disparan, así
      // que reposar con una onda en curso se come el flanco de BUSY: la espera
      // vence sus 20 ms, vuelve sin esperar, y el cuadro siguiente se escribe
      // encima de uno que el panel todavía está dibujando. Son dos segundos como
      // mucho, una sola vez por refresco, y es lo que separa una pantalla limpia
      // de una manchada.
      porQue = "el panel está refrescando";
    const bool restBlocked = porQue != nullptr;
    {
      static const char* ultimoPorQue = nullptr;
      // Sólo interesa mientras el aparato está ocioso: bloquearlo mientras
      // alguien lo usa es lo normal y llenaría el log.
      const bool ocioso = millis() - lastActivityTime >= IdleSleep::REST_AFTER_MS;
      if (ocioso && porQue != ultimoPorQue) {
        ultimoPorQue = porQue;
        if (porQue)
          LOG_INF("MAIN", "el reposo no entra: %s", porQue);
        else
          LOG_INF("MAIN", "ya nada bloquea el reposo");
      }
    }
    // Red de seguridad. Hasta 1.5.71 sólo corría con el tiempo en "nunca"
    // (sleepTimeoutMs == 0); con el valor forzado de la ws397 eso ya no puede
    // pasar, así que quedaría muerta justo cuando más hace falta. Ahora mide
    // contra `lastUserInputTime`, que NO lo reinician ni `preventAutoSleep()`
    // ni la música: si nadie tocó el aparato en media hora, no está enchufado y
    // el reposo sigue bloqueado, se duerme igual y queda dicho por qué.
    // Cubre lo que el auto-sleep no puede cubrir: una Activity que pide
    // "no duermas" para siempre (OpdsBookBrowserActivity lo hace) congela el
    // contador de ocio y con él el auto-sleep de los diez minutos.
    //
    // LA COMPUERTA TAMBIÉN TIENE QUE MEDIR CONTRA `lastUserInputTime`. Medía
    // contra `lastActivityTime`, que unas líneas más arriba —en ESTA misma
    // pasada del loop— lo reinician `preventAutoSleep()`, la música y PWR. O
    // sea que cuando el reposo estaba bloqueado POR alguna de esas tres, la
    // compuerta valía ~0 ms, nunca abría, y `restBlockedSince` se reiniciaba en
    // el `else`: la red no armaba jamás. Y los únicos bloqueos que SÍ la abrían
    // (WiFi arriba, skipLoopDelay) ya los agarra el auto-sleep de los diez
    // minutos, o sea antes. Era código muerto, y justo para el caso que dice
    // cubrir. Los dos plazos miden lo mismo ahora: lo último que hizo una
    // persona.
    static unsigned long restBlockedSince = 0;
    static const char* motivoDelBloqueo = nullptr;
    if (restBlocked && !cablePuesto && millis() - lastUserInputTime >= IdleSleep::REST_AFTER_MS) {
      // Si CAMBIÓ el motivo, el contador arranca de nuevo: cada causa tiene su
      // propio plazo (REV-062) y mezclarlos daría lo peor de los dos — dos
      // horas de música seguidas de otra cosa vencerían al instante, y media
      // hora de otra cosa seguida de música se llevaría las tres horas.
      if (restBlockedSince == 0 || porQue != motivoDelBloqueo) {
        restBlockedSince = millis();
        motivoDelBloqueo = porQue;
      }
      // REV-062: el plazo depende de QUÉ lo bloquea. Ver `MUSIC_GIVE_UP_MS`.
      const bool soloLaMusica = porQue == RAZON_MUSICA;
      const unsigned long plazo = soloLaMusica ? MUSIC_GIVE_UP_MS : REST_BLOCKED_GIVE_UP_MS;
      if (millis() - restBlockedSince >= plazo && millis() - lastUserInputTime >= plazo) {
        if (soloLaMusica) {
          LOG_ERR("MAIN", "la música lleva %lu ms sonando sin que nadie toque el aparato: se duerme igual",
                  millis() - restBlockedSince);
        } else {
          LOG_ERR("MAIN",
                  "el reposo lleva %lu ms bloqueado por «%s» y nadie toca el aparato hace %lu ms: se "
                  "duerme igual",
                  millis() - restBlockedSince, porQue ? porQue : "?", millis() - lastUserInputTime);
        }
        restBlockedSince = 0;
        enterDeepSleep(true);
        return;
      }
    } else {
      restBlockedSince = 0;
      motivoDelBloqueo = nullptr;
    }

    // El reposo NO puede durar más que lo que falta para el deep sleep. Sin
    // esto el aparato se queda en light sleep para siempre y nunca baja al
    // sueño profundo: el ocio sólo crece mientras el loop corre, y reposando no
    // corre. Hasta 1.5.71 el ciclo de 2 s del acelerómetro devolvía el control
    // todo el tiempo y lo tapaba; al sacar ese sondeo quedó a la vista.
    unsigned long cap = msUntilNextAlarm();
    // UN VENCIDO QUE ESTA PANTALLA NO VA A ATENDER NO PUEDE APAGAR EL REPOSO.
    // `msUntilNextAlarm()` devuelve 1 ms cuando hay algo vencido, y con un tope
    // de 1 ms `IdleSleep::tick()` no reposa (MIN_REST_MS son 500). Eso está bien
    // mientras la alarma esté por sonar — pero el lector está excluido A
    // PROPÓSITO de `checkTimeAlarms()`, así que un recordatorio que venció
    // leyendo deja el tope en 1 ms PARA SIEMPRE y el aparato se queda a 40 mA
    // sin reposar nunca, hasta el auto-sleep de los diez minutos. Y vuelve a
    // pasar con cada repique. Si la pantalla de turno no lo va a atender,
    // reposar no lo hace más tarde de lo que ya está: el tope no aplica.
    if (cap > 0 && cap < IdleSleep::MIN_REST_MS && !alarmWouldRingHere()) cap = 0;
    if (sleepTimeoutMs > 0) {
      const unsigned long ocio = millis() - lastActivityTime;
      const unsigned long faltaParaDormir = sleepTimeoutMs > ocio ? sleepTimeoutMs - ocio : 1;
      if (cap == 0 || faltaParaDormir < cap) cap = faltaParaDormir;
    }
    IDLE_SLEEP.capNextRest(cap);
    switch (IDLE_SLEEP.tick(millis() - lastActivityTime, restBlocked)) {
      case IdleSleep::Woke::Button:
        // El botón que despertó se lee en la pasada siguiente (la entrada de
        // ESTA pasada se leyó antes de dormir): salir ya y empezar de nuevo.
        // Y esa pulsación cuenta como TOQUE pero no como MANTENIDO: quien
        // despierta el aparato apretando Atrás y lo sigue apretando porque la
        // pantalla no reacciona no está pidiendo sincronizar (Atrás mantenido
        // 1,2 s en el hub = levantar la red). Se marca en la pasada siguiente,
        // cuando la tecla ya se leyó (restWakeHeldPending).
        restWakeHeldPending = true;
        lastActivityTime = millis();
        powerManager.setPowerSaving(false);
        return;
      case IdleSleep::Woke::Rejected:
        // Un pin ya estaba en bajo. NO se toca lastActivityTime: si esto se
        // tratara como actividad, un pin trabado dejaría al aparato despierto
        // para siempre, sin reposar y sin llegar nunca al deep sleep.
        break;
      case IdleSleep::Woke::Timer:
        // Sigue reposando: nada que pintar, y el deep sleep se decide arriba en
        // la pasada siguiente con el ocio ya más grande.
        return;
      case IdleSleep::Woke::NotSlept:
        break;
    }
  }

  // A hold that woke the device (or, on the ws397, a PWR hold that powered the
  // PMIC on and was still down when the key decoder started) must be released
  // before it can count as a new in-app long press. Otherwise a user who keeps
  // holding after wake would put the device straight back to sleep once
  // allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!powerHeld) powerReleasedSinceWake = true;

  const bool powerGateOpen = powerReleasedSinceWake && millis() >= allowSleepAt;

  if (usePowerHoldTiers()) {
    // ws397: apretar y soltar = suspender; mantenido = barrita; a los 3 s apaga.
    if (handlePowerHold(powerGateOpen)) {
      delay(10);  // banner on screen: no need to spin
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

  checkVoiceShortcut();
  checkMotionGestures();

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // Una pantalla que se abrió sola y se resolvió sola pide dormir en vez de
  // devolver el aparato al hub (ver util/SleepRequest.h). Se atiende ACÁ, con
  // el loop de la Activity ya terminado, y no adentro de ella: enterDeepSleep()
  // corre el onExit() de la pantalla de turno y no puede hacerlo desde su
  // propio loop().
  if (sleepreq::take()) {
    LOG_INF("MAIN", "a dormir a pedido de %s", activityManager.currentActivityName());
    enterDeepSleep();
    return;  // no se llega: enterDeepSleep termina en esp_deep_sleep_start
  }

  // El loop volvió: ésta es la señal de vida que mira el supervisor (REV-065).
  // El bombeo de red da la suya cada 25 ms, así que una descarga de noventa
  // segundos late igual sin que la pasada termine.
  loopwdt::beat();

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // UNA PASADA DE MEDIO MINUTO NO ES UNA LÍNEA DE DEPURACIÓN. Hasta acá lo
  // único que quedaba de una descarga de 90 s era un `[DBG] New max loop
  // duration`, que sale sólo cuando el número es un récord y no dice qué lo
  // causó: el dueño tuvo que contar que "se traba y no responde al botón de
  // atrás ni al de suspender". La regla de 1.5.96 es que eso lo detecta el
  // aparato. Sale SIEMPRE que la pasada se pase del plazo (no sólo la primera
  // vez), dice qué operación de red la ocupó y CUÁNTAS veces se bombeó
  // mientras tanto: ese número es la prueba de que PWR y Atrás se estuvieron
  // atendiendo. Bombeos ~0 con una operación larga = el bombeo no llegó a ese
  // camino, y hay que ir a buscarlo ahí.
  //
  // Dos plazos distintos y a propósito. Con la red, CUATRO segundos ya es una
  // pasada que hay que explicar. Sin la red, el piso son OCHO: una página con
  // imagen la primera vez cuesta 4,8 s (1.5.83) y un FULL del panel 2,2 s, y
  // convertir eso en un [ERR] por página sería la línea por pintada que hubo
  // que sacar en 1.5.93.
  constexpr unsigned long LOOP_STALL_NET_MS = 4000;
  constexpr unsigned long LOOP_STALL_OTHER_MS = 8000;
  const unsigned long netMs = netpump::passMs();
  const bool fueLaRed = netMs * 2 >= loopDuration;
  if (fueLaRed && loopDuration >= LOOP_STALL_NET_MS) {
    LOG_ERR("LOOP",
            "el loop estuvo %lu ms sin atender a nadie: lo tuvo la red en «%s» (%lu ms), "
            "se bombeó %lu veces (PWR y Atrás siguieron vivos)",
            loopDuration, netpump::passWhat(), netMs, static_cast<unsigned long>(netpump::passTicks()));
  } else if (!fueLaRed && loopDuration >= LOOP_STALL_OTHER_MS) {
    LOG_ERR("LOOP",
            "el loop estuvo %lu ms sin atender a nadie y NO fue la red (%lu ms de red): "
            "pantalla %s. Eso no tiene bombeo y hay que ir a buscarlo ahí",
            loopDuration, netMs, activityManager.currentActivityName());
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
