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
#include <PowerManager.h>
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
#include <soc/soc_caps.h>
#if SOC_PM_SUPPORT_EXT1_WAKEUP
#include <driver/rtc_io.h>
#endif
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/AudioTestActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "components/Selection.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/PowerKey.h"
#include "util/Shtc3.h"
#include "util/BatteryLog.h"
#include "util/IdleSleep.h"
#include "util/RtcAlarm.h"
#include "input/MotionInput.h"
#include "util/ScreenshotUtil.h"
#include "music/MusicPlayer.h"
#include "voice/VoiceRecorder.h"
#include "TaskConfig.h"

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
constexpr unsigned long POWER_HOLD_ACTION_MS = 1200;  // aparece la barrita
constexpr unsigned long POWER_HOLD_SLEEP_MS = POWER_HOLD_ACTION_MS;  // soltar acá o después: suspende
constexpr unsigned long POWER_HOLD_WARN_MS = 2300;    // segundo cartel: está por apagarse
constexpr unsigned long POWER_HOLD_OFF_MS = 3000;     // apagar de verdad
}  // namespace

// A wake hold must never become an in-app button action. Boot may continue
// while the wake key is held; swallow the one release that ends that wake
// gesture. The wake key is the power button on most boards and OK on the
// ws397 (its PWR key sits behind the PMIC and cannot wake the chip), so this
// watches HalGPIO::BTN_CONFIRM there — see wakeKeyIndex().
static bool wakeKeyReleasePending = false;

static uint8_t wakeKeyIndex() {
  return BoardConfig::isWS397() ? HalGPIO::BTN_CONFIRM : HalGPIO::BTN_POWER;
}

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
static void armReminderWake(const bool quiet = false) {
  if (reminderWakeArmed) return;
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) {
    // Not latched: the second caller gets another go at the shared I2C bus.
    if (!quiet) LOG_ERR("MAIN", "no clock: nothing armed, the timer will not ring asleep");
    return;
  }
  reminderWakeArmed = true;
  const time_t due = nextWakeInstant(now);
  if (due == 0) return;
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
  // ws397: the wake key is OK (GPIO5, RTC-capable, EXT1 low). PWR cannot wake:
  // the PMIC IRQ is on GPIO38, which is not an RTC GPIO. A PWR press while
  // asleep only latches status in the AXP2101 (flushed by PowerKey::begin()
  // on the next boot); a 10 s PWR hold makes the PMIC cut the rails (PressOff,
  // the deliberate hardware escape), after which PWR held 1 s powers it back on.
  const int8_t wakePin = freeink::PowerManager::wakeSourcePin();
  if (wakePin < 0) {
    LOG_ERR("MAIN", "no wake pin in the board profile: only the timer can wake the device");
  }
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  else if (!rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(wakePin))) {
    // The SDK refuses to arm it (and says so only on the serial console).
    LOG_ERR("MAIN", "wake pin GPIO%d is not an RTC GPIO: the button will NOT wake the device", wakePin);
  }
#endif
  powerManager.startDeepSleep(gpio);
}

// ws397: el temporizador y los recordatorios tienen que sonar aunque el aparato
// esté despierto en otra pantalla. Antes solo los miraba el tick del hub y el
// arranque después de dormir, así que un temporizador vencido en Notas, Agenda o
// Ajustes no sonaba nunca. Se dispara sobre las pantallas tranquilas; el lector y
// las que usan red o audio se dejan en paz (ahí manda el wake por deep sleep).
constexpr unsigned long DOUBLE_BACK_MS = 500;  // ventana del doble toque de Atrás
// Media hora de ocio sin poder reposar ni una vez: algo lo está bloqueando y en
// "siempre encendido" nadie más va a mandar a dormir. Ver la red de seguridad
// en el loop.
constexpr unsigned long REST_BLOCKED_GIVE_UP_MS = 30UL * 60UL * 1000UL;
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

// Devuelve true cuando puso una alarma en pantalla: el llamador tiene que
// tratar eso como actividad, o el reposo se lo lleva puesto antes de pintarlo.
static bool checkTimeAlarms() {
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop()) return false;
  if (busyRecording()) return false;
  // preventAutoSleep() es "esta pantalla esta ocupada AHORA": red arriba,
  // hablando, descargando. Es el mismo criterio con el que el reposo decide no
  // dormir, asi que sirve igual para no pisar una transferencia a medio camino.
  if (activityManager.preventAutoSleep()) return false;
  if (isAlarmSilentScreen(activityManager.currentActivityName())) return false;
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
    activityManager.pushActivity(std::make_unique<TimerActivity>(renderer, mappedInputManager, 0, /*resumeFired=*/true));
    return true;
  }
  if (const HubStore::Reminder* due = HUB_STORE.dueReminder(now)) {
    LOG_INF("MAIN", "suena el recordatorio %d desde %s", due->id, activityManager.currentActivityName());
    activityManager.pushActivity(
        std::make_unique<ReminderAlertActivity>(renderer, mappedInputManager, due->id, due->title, due->when));
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
  if (due > now) {
    RTC_ALARM.armAt(due, now);
  } else if (RTC_ALARM.armedAt() != 0) {
    RTC_ALARM.disarm();
  }
  if (due == 0) return 0;
  if (due <= now) return 1;
  const time_t seconds = due - now;
  if (seconds > 3600) return 0;
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
    activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInputManager));
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

// APAGAR, no dormir. Deja el fondo de pantalla puesto (el panel es biestable:
// lo que queda pintado se queda pintado con el aparato muerto), desmonta la
// tarjeta y le pide al PMIC que corte los rieles. Vuelve sólo si el PMIC no
// contestó, para que el llamador se conforme con dormir.
static void powerOffNow() {
  MUSIC.stop();
  HUB_STORE.saveToFile();
  APP_STATE.showBootScreen = false;
  APP_STATE.saveToFile();
  paintWallpaperForSleep();
  devlog::event("MAIN", "apagado por PWR mantenido");
  devlog::close();
  Storage.prepareForDeepSleep();
  if (!POWER_KEY.powerOff()) {
    LOG_ERR("MAIN", "el PMIC no aceptó el apagado: se duerme");
    return;
  }
  // El corte no es instantáneo: el PMIC baja los rieles en unos ms.
  delay(500);
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

  // El diario de la batería, con la tarjeta todavía montada: esta es la muestra
  // que cierra el tramo despierto y abre el tramo dormido, que es el largo y el
  // que de verdad dice cuánto dura.
  batterylog::sampleNow("antes de dormir");

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
  const int boxW = screenW - 72;
  const int boxH = 118;
  const int x = (screenW - boxW) / 2;
  const int y = screenH / 2 - boxH / 2;

  renderer.fillRoundedRect(x, y, boxW, boxH, 16, Color::White);
  renderer.drawRoundedRect(x, y, boxW, boxH, 3, 16, true);

  const StrId what = aboutToSleep ? StrId::STR_PWR_HOLD_SLEEPING : StrId::STR_PWR_HOLD_OFF;
  renderer.drawCenteredText(UI_12_FONT_ID, y + 30, I18N.get(what), true, EpdFontFamily::BOLD);

  // Bar: how much is left until sleep.
  const int barX = x + 24;
  const int barW = boxW - 48;
  const int barY = y + 76;
  constexpr int barH = 16;
  renderer.drawRect(barX, barY, barW, barH, 2, true);
  const int filled = static_cast<int>(barW * std::min(held, POWER_HOLD_OFF_MS) / POWER_HOLD_OFF_MS);
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

  if (pressed) {
    // No sleep permission yet (just woke / just booted) or DOWN held (screenshot
    // combo): the hold is not ours.
    if (!gateOpen || gpio.isPressed(HalGPIO::BTN_DOWN)) return false;
    if (held >= POWER_HOLD_OFF_MS) {
      LOG_INF("MAIN", "PWR mantenido %lu ms: se apaga", held);
      bannerStage = 0;
      powerOffNow();
      // Si el PMIC no contestó, no queda colgado: se duerme, que es lo de antes.
      enterDeepSleep();
      return true;
    }
    if (bannerStage == 0 && held >= POWER_HOLD_ACTION_MS) {
      bannerStage = 1;
      POWER_KEY.consumeHold();  // the release after the bar is not a short press
      drawPowerHoldBanner(held, false);
    } else if (bannerStage == 1 && held >= POWER_HOLD_WARN_MS) {
      bannerStage = 2;
      drawPowerHoldBanner(held, true);  // second and last repaint: ink is expensive
    }
    return bannerStage != 0;
  }

  // El toque corto ya no hace nada (el menú de pantalla se sacó), pero el flag
  // es un latch: se vacía acá para que no quede colgado esperando a alguien.
  POWER_KEY.tookShortPress();
  if (bannerStage == 0) return false;
  const unsigned long lastHold = held;  // heldMs() guarda el largo del hold que terminó
  bannerStage = 0;
  if (lastHold >= POWER_HOLD_SLEEP_MS) {
    LOG_DBG("MAIN", "PWR soltado a los %lu ms: a dormir", lastHold);
    enterDeepSleep();
    return true;  // no se llega: enterDeepSleep termina en esp_deep_sleep_start
  }
  // Soltado antes de 1,2 s: no pasa nada, la barrita se va.
  activityManager.requestUpdate();
  return true;
}

// El MENÚ DE PANTALLA se sacó: PWR hace una sola cosa, mantenerlo. Con él se
// fueron "limpiar" y "bloquear", que eran sus otras dos entradas — limpiar ya lo
// hace solo el coordinador de refresco (un completo cada 12 parciales) y el
// bloqueo se quedó sin puerta; si hace falta, entra en Ajustes → Sistema.

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
  // Después de los botones y del IMU: prueba los pines que despiertan del reposo.
  IDLE_SLEEP.begin();
  RTC_ALARM.begin();
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
  SETTINGS.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  devlog::begin();  // from here every LOG_* line also goes to the SD
  setLogSink(&devlog::write);
  POWER_KEY.logSnapshot();  // the PMIC register dump, now that it reaches /board/log
  // ws397: the short-press binding is meaningless here (PWR short = clean
  // screen, and OK is plain Confirm), and SLEEP would make a 10 ms wake tap
  // count as verified. Force it whatever the file (or the web) says.
  if (BoardConfig::isWS397()) SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
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

  if (BoardConfig::isWS397()) {
    // OK is plain Confirm here (DigitalButtons): the shared confirm/power
    // toggle does not apply, and the short-press binding stays IGNORE even if
    // the web settings page writes something else at runtime.
    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
  } else {
    gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  }
  mappedInputManager.update();
  POWER_KEY.pump();  // ws397: PMIC key state for this pass (no-op elsewhere)
  MOTION.poll();     // ws397: acelerómetro cada 80 ms (no-op sin IMU o sin gestos)
  shtc3::tick();     // temperatura de adentro, en dos tiempos y sin bloquear
  batterylog::tick();  // el diario de la batería, una línea cada diez minutos

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
  const bool userInput = (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
                                           halTiltSensor.hadActivity());
  if (userInput || activityManager.preventAutoSleep() || MUSIC.isActive() || POWER_KEY.pressed()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Music and alarms run BEFORE any early return below (wake release,
  // screenshot combo, hold banner): holding PWR must not freeze the track or
  // silence a reminder. The music player lives outside the Activity; this is
  // what chains the next track when one ends, wherever the user is.
  MUSIC.pump();
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
      lastActivityTime = millis();
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

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
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
    const bool cablePuesto = POWER_KEY.vbusPresent() || gpio.isUsbConnected();
    // Queda en el log para poder confirmarlo sin cable: si VBUS dice una cosa y
    // "está cargando" otra, es justamente el caso que rompía el reposo.
    static int lastCableState = -1;
    const int cableState = cablePuesto ? 1 : 0;
    if (cableState != lastCableState) {
      lastCableState = cableState;
      LOG_INF("MAIN", "cable %s (vbus=%d cargando=%d)", cablePuesto ? "puesto" : "sacado",
              POWER_KEY.vbusPresent() ? 1 : 0, gpio.isUsbConnected() ? 1 : 0);
    }
    const bool restBlocked = activityManager.preventAutoSleep() || activityManager.skipLoopDelay() ||
                             MUSIC.isActive() || busyRecording() || POWER_KEY.pressed() || cablePuesto ||
                             WiFi.getMode() != WIFI_MODE_NULL;
    // Red de seguridad del modo "siempre encendido" (sleepTimeoutMs == 0): ahí
    // nadie va a mandar el aparato a dormir, así que si algo bloquea el reposo
    // de forma permanente —la red que quedó arriba, el menú abierto— la batería
    // se termina en una noche sin que nadie se entere. A la media hora de ocio
    // sin haber podido reposar ni una vez, se duerme igual y queda dicho en el
    // log por qué. Enchufado no aplica: ahí la batería no es el problema.
    static unsigned long restBlockedSince = 0;
    if (restBlocked && !cablePuesto && millis() - lastActivityTime >= IdleSleep::REST_AFTER_MS) {
      if (restBlockedSince == 0) restBlockedSince = millis();
      if (sleepTimeoutMs == 0 && millis() - restBlockedSince >= REST_BLOCKED_GIVE_UP_MS) {
        LOG_ERR("MAIN", "siempre encendido: el reposo lleva %lu ms bloqueado, se duerme igual",
                millis() - restBlockedSince);
        restBlockedSince = 0;
        enterDeepSleep(true);
        return;
      }
    } else {
      restBlockedSince = 0;
    }

    IDLE_SLEEP.capNextRest(msUntilNextAlarm());
    switch (IDLE_SLEEP.tick(millis() - lastActivityTime, restBlocked)) {
      case IdleSleep::Woke::Button:
      case IdleSleep::Woke::Motion:
        // El botón que despertó se lee en la pasada siguiente (la entrada de
        // ESTA pasada se leyó antes de dormir): salir ya y empezar de nuevo.
        lastActivityTime = millis();
        powerManager.setPowerSaving(false);
        return;
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
    // ws397: PWR mantenido = barrita; soltar suspende, llegar a los 3 s apaga.
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
