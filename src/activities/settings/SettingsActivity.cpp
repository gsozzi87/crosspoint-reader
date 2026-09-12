#include "SettingsActivity.h"
#include <ws397_version.h>  // ws397: build number lives here, not in a -D flag

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>  // bitmapFromIcon: la pestaña del resalte va como marcador de la lista
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AudioTestActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "activities/home/AssetSyncActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "ServerTestActivity.h"
#include "HubStore.h"
#include "voice/UiSound.h"
#include "activities/home/HubLocationActivity.h"
#include "activities/home/PhotosActivity.h"
#include "activities/home/HubSyncActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "music/MusicPlayer.h"
#if FREEINK_CAP_USB_MSC
#include "activities/network/UsbDriveActivity.h"
#include "DevicePairActivity.h"
#include <HalTiltSensor.h>

#include "TaskStatsActivity.h"
#include "MotionActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
// La pestaña negra de 5 px del resalte (components/Selection.h) en formato de
// icono del SDK, para que la lista de fui la dibuje como marcador de la fila
// elegida: 1 bpp, bit 0 = tinta, asi que la fila entera de ceros es tinta y
// solo se usan los primeros 5 bits de cada byte.
constexpr uint8_t kSelectionTabBits[26] = {0};
constexpr freeink::Icon kSelectionTab = {5, 26, 13, kSelectionTabBits};
}  // namespace

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Settings", renderer, mappedInput) {}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // En la ws397 buena parte del menú de upstream no tiene con qué funcionar: no
  // hay teclado (todo entra por voz), no se sincroniza con KOReader, no se
  // navegan servidores OPDS, el firmware entra por OTA desde nuestro servidor
  // (nunca desde la SD) y la prueba de servidor la reemplazó la sincronización
  // del hub. Nada de eso se borra: se esconde acá, en la lista del aparato, así
  // el código upstream y la API web siguen intactos.
  const bool isWs397 = BoardConfig::ACTIVE.board == BoardConfig::Board::WS397;

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // The sunlight fading fix is a grayscale-waveform compensation that does
      // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
      if (setting.valuePtr == &CrossPointSettings::fadingFix &&
          (BoardConfig::isX4Pro() || BoardConfig::isX4Classic())) {
        continue;
      }
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      // OK es confirmar y encender a la vez (DigitalConfirmPowerHold): elegir
      // "Dormir" acá dejaba al aparato sin botón de confirmar, y las demás
      // opciones dependen de que el pulso corto emita Power, que solo pasa con
      // "Dormir". Se esconde el ajuste (y el de las notas al pie que cuelga de
      // él); en la web sigue estando por si hay que rescatarlo.
      if (isWs397 && (setting.valuePtr == &CrossPointSettings::shortPwrBtn ||
                      setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack)) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      // "Tiempo hasta dormir" es un número de 1 a 31 donde 31 quiere decir
      // "nunca", que no hay forma de adivinar mirando la pantalla. En la ws397
      // se esconde y en su lugar va el modo de energía de abajo, con nombres;
      // en la web y en las demás placas el ajuste sigue igual.
      if (isWs397 && setting.valuePtr == &CrossPointSettings::sleepTimeoutMinutes) continue;
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  if (!isWs397) {
    // Sincronización de KOReader y servidores OPDS: nada de eso se usa acá.
    systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // OTA fetches this board's own release asset (see OtaUpdater); boards whose
  // asset isn't published yet just report no update available.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  // ws397: el "botón de bajar adjuntos". Todo lo pesado (dibujos y audios de las
  // tarjetas, sonidos, la Biblia entera) vive en el servidor y se baja acá, o
  // solo, detrás de la actualización de firmware.
  if (isWs397) {
#if FREEINK_CAP_USB_MSC
    // La tarjeta como disco por USB: es la forma de cargar libros y MP3 sin
    // sacarla del aparato, así que va acá arriba y no escondida en Transferir
    // archivos.
    systemSettings.push_back(SettingInfo::Action(StrId::STR_USB_DRIVE, SettingAction::UsbDrive));
#endif
    // "Descargar contenido" salió del menú (1.5.68): el paquete se baja solo al
    // sincronizar cuando hay algo nuevo, y después de cada actualización.
  }
  // Actualizar por SD: en la ws397 el firmware entra por OTA desde el servidor
  // propio, así que la entrada solo confunde.
  if (!isWs397) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // Distribuciones de teclado: este aparato nunca tiene teclado, todo entra por voz.
  if (!isWs397) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts));
  }
  // La prueba de audio salió del menú (1.5.68): "se va, ya no tiene sentido".
  // AudioTestActivity queda en el código por si hay que volver a colgarla.
  if (isWs397) {
    // "Prueba de servidor" era un diagnóstico de desarrollo: lo mismo lo dice
    // Sincronizar hub, que además sirve para algo. Queda ServerTestActivity en
    // el código por si hay que volver a colgarla de algún lado.
    // Vincular con la cuenta de la web: el aparato muestra un código de seis
    // dígitos y la persona lo escribe desde el teléfono, ya con su sesión
    // iniciada. Es la única forma de asociarlo sin teclado.
    systemSettings.push_back(SettingInfo::Action(StrId::STR_PAIR_TITLE, SettingAction::DevicePair));
    // Sincronizar hub salió del menú (1.5.68): queda sólo por botón (Atrás
    // mantenido en el hub), y la ayuda de abajo del hub lo explica.
    // Gestos del IMU: encender o apagar, ver qué lee el sensor y calibrar cómo
    // está montado (sin eso, "inclinar a la derecha" puede ser cualquier eje).
    if (halTiltSensor.isAvailable()) {
      systemSettings.push_back(SettingInfo::DynamicEnum(
                                   StrId::STR_MOTION_GESTURES, {StrId::STR_MOTION_OFF, StrId::STR_MOTION_ON},
                                   []() -> uint8_t { return HUB_STORE.motionGestures ? 1 : 0; },
                                   [](uint8_t value) {
                                     HUB_STORE.motionGestures = value != 0;
                                     HUB_STORE.saveToFile();
                                   })
                                   .withSwitch());
      systemSettings.push_back(SettingInfo::Action(StrId::STR_MOTION_TITLE, SettingAction::Motion));
    }
    // MODO DE ENERGIA: en esta placa no se elige. Elegir entre "ahorro" y
    // "normal" era pedirle al usuario que decidiera algo que no puede evaluar
    // —la diferencia son cinco minutos de espera antes de dormir— y la respuesta
    // correcta es siempre la misma. Queda fijo en ahorro (5 min) y el ajuste no
    // se muestra. En las demás placas sigue igual.
    if (!isWs397) {
      systemSettings.push_back(SettingInfo::DynamicEnum(
          StrId::STR_POWER_MODE, {StrId::STR_POWER_SAVER, StrId::STR_POWER_NORMAL, StrId::STR_POWER_ALWAYS_ON},
          []() -> uint8_t {
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) return 2;
            return SETTINGS.sleepTimeoutMinutes <= 5 ? 0 : 1;
          },
          [](const uint8_t value) {
            SETTINGS.sleepTimeoutMinutes = value == 0   ? static_cast<uint8_t>(5)
                                           : value == 1 ? static_cast<uint8_t>(10)
                                                        : CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES;
            SETTINGS.saveToFile();
          }));
    } else if (SETTINGS.sleepTimeoutMinutes != 5) {
      SETTINGS.sleepTimeoutMinutes = 5;
      SETTINGS.saveToFile();
    }
    // La contracara de src/TaskConfig.h: acá se ve cuánto stack usó de verdad
    // cada tarea contra lo que tiene declarado, y cómo va el heap interno.
    systemSettings.push_back(SettingInfo::Action(StrId::STR_SETTING_MEMORY, SettingAction::Memory));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_HUB_LOCATION, SettingAction::HubLocation));
    // Fondo de pantalla: elegir qué foto queda pintada cuando el aparato se suspende.
    systemSettings.push_back(SettingInfo::Action(StrId::STR_WALLPAPER, SettingAction::Wallpaper));
    systemSettings.push_back(SettingInfo::DynamicEnum(
        StrId::STR_SPEAK_MODE, {StrId::STR_SPEAK_NEVER, StrId::STR_SPEAK_SHORT, StrId::STR_SPEAK_ALWAYS},
        [] { return HUB_STORE.speakMode; },
        [](uint8_t v) {
          HUB_STORE.speakMode = v;
          HUB_STORE.saveToFile();
        }));
    // El volumen del aparato (música, voz de Piper y avisos son uno solo). Se
    // pidió tenerlo también acá: hasta 1.5.43 sólo se podía tocar desde la
    // música o desde la web, y nadie lo encontraba.
    systemSettings.push_back(SettingInfo::DynamicValue(
        StrId::STR_MUSIC_VOLUME, {0, 100, 10}, [] { return static_cast<uint8_t>(MUSIC.volume()); },
        [](uint8_t v) { MUSIC.setVolume(v); }));
    // Sonidos de la interfaz: clics cortos al navegar, elegir, volver y pasar
    // página. De fábrica apagados; al elegir un nivel suena el clic para que se
    // escuche en el momento cuánto es "suave" y cuánto "normal".
    systemSettings.push_back(SettingInfo::DynamicEnum(
        StrId::STR_UI_SOUNDS, {StrId::STR_UI_SOUNDS_OFF, StrId::STR_UI_SOUNDS_SOFT, StrId::STR_UI_SOUNDS_NORMAL},
        [] { return HUB_STORE.uiSoundMode; },
        [](uint8_t v) {
          HUB_STORE.uiSoundMode = v;
          HUB_STORE.saveToFile();
          UI_SOUND.play(uisound::Sound::Select);
        }));
  }
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  // Reset selection to first category (ring position 0, the tab bar, comes
  // from the base's per-tab nav reset)
  selectedCategoryIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  activeNav().top = 0;  // category switches start the list at the top (no per-tab memory here)
  rebuildRowItems();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for *currentSettings.
// Structural — call only when the active category or a category's setting
// list changes, never from buildScreen(), which only refreshes rowValues_
// content and rowItems_[].value pointers in place.
void SettingsActivity::rebuildRowItems() {
  const auto& settings = *currentSettings;
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(settings[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    // Los si/no van con interruptor dibujado en vez de la palabra "Activado":
    // se reconoce del telefono sin leerlo. Estructural (que la fila SEA un
    // interruptor); el estado lo pone buildScreen en cada pasada.
    item.toggle = settings[i].type == SettingType::TOGGLE || settings[i].switchStyle;
    rowItems_.push_back(item);
  }
}

void SettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  selectCategory(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  (void)index;  // toggleCurrentSetting reads the ring position
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  toggleCurrentSetting();
  // Tap-first: a tapped row is not a cursor position. Leaving it focused
  // (inverted) after the tap meant the row stayed black once its sub-screen or
  // popup closed, and Back then had to clear that focus before a second Back
  // left Settings. Hand the focus back to the tab band; the viewport stays put.
  if (mappedInput.hasTouch()) {
    activeNav().selected = 0;
  }
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  UITheme::getInstance().reload();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::stepTab(const int direction) {
  // Ring position 0 stays on the tab bar; a row selection collapses to the
  // new category's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  selectedCategoryIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                        : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
  selectCategory(selectedCategoryIndex);
  activeNav().selected = onTabBar ? 0 : 1;
  requestUpdate();
}

bool SettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      toggleCurrentSetting();
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (ringPos() > 0) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }

  return false;
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = ringPos() - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
                         applyUiSettingChange(valuePtr);
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr == nullptr && setting.valueGetter) {
    // Valor dinámico (el volumen): mismo ciclo min..max, pero leído y escrito
    // por los lambdas en vez de por un puntero a CrossPointSettings.
    const int current = setting.valueGetter();
    const int next = current + setting.valueRange.step;
    setting.valueSetter(next > setting.valueRange.max ? setting.valueRange.min : static_cast<uint8_t>(next));
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::AudioTest:
        startActivityForResult(std::make_unique<AudioTestActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ServerTest:
        startActivityForResult(std::make_unique<ServerTestActivity>(renderer, mappedInput), resultHandler);
        break;
#if FREEINK_CAP_USB_MSC
      case SettingAction::UsbDrive:
        startActivityForResult(std::make_unique<UsbDriveActivity>(renderer, mappedInput), resultHandler);
        break;
#endif
      case SettingAction::DevicePair:
        startActivityForResult(std::make_unique<DevicePairActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Memory:
        startActivityForResult(std::make_unique<TaskStatsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Motion:
        startActivityForResult(std::make_unique<MotionActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::HubSync:
        startActivityForResult(std::make_unique<HubSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::HubLocation:
        startActivityForResult(std::make_unique<HubLocationActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Wallpaper:
        startActivityForResult(std::make_unique<PhotosActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadAssets:
        startActivityForResult(std::make_unique<AssetSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 // TextSettingsActivity saves on each change; no save needed here.
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  applyUiSettingChange(setting.valuePtr);
  activeNav().selected = std::min(ringPos(), settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

// Un ajuste que abre OTRA pantalla lleva galon a la derecha; el que cambia un
// valor ahi mismo muestra el valor. Es la unica diferencia entre las dos
// clases de fila y hasta 1.5.47 no se veia en ningun lado.
const char* const SettingsActivity::chevronGlyph = "\xE2\x80\xBA";  // U+203A

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.type == SettingType::ACTION) {
    return chevronGlyph;
  }
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // La fila lo dibuja como interruptor; el texto queda de respaldo para el
    // caso raro de un TOGGLE sin puntero, que cae abajo en "".
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    if (value >= setting.enumValues.size()) return "";
    return I18N.get(setting.enumValues[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) {
      return I18N.get(setting.enumValues[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr == nullptr && setting.valueGetter) {
    return std::to_string(setting.valueGetter()) + " %";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints. El margen de
  // abajo lleva verticalSpacing además del alto de los hints (misma cuenta que
  // TextSettingsActivity y WifiSelectionActivity): con el hueco justo, la última
  // fila terminaba pegada a la barra de botones y parecía tapada por ella.
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  buildTabBar(screen);

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the
  // category was last selected/rebuilt; only the live value text needs
  // refreshing here, by assigning into the existing rowValues_ strings (no
  // vector growth) rather than building a new items/values vector on every
  // render.
  const auto& settings = *currentSettings;
  for (size_t i = 0; i < settings.size(); i++) {
    if (rowItems_[i].toggle) {
      rowItems_[i].toggleChecked = settings[i].valuePtr != nullptr ? SETTINGS.*(settings[i].valuePtr) != 0
                                   : settings[i].valueGetter    ? settings[i].valueGetter() != 0
                                                                : false;
      rowValues_[i].clear();
      rowItems_[i].value = nullptr;
      continue;
    }
    rowValues_[i] = settingValueText(settings[i]);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Jerarquia dentro de la fila: el NOMBRE en el cuerpo de la lista (UI_12) y
  // el VALOR mas chico a la derecha (UI_10), en el mismo renglon. Hasta 1.5.47
  // los dos iban en UI_10 y la fila no tenia jerarquia ninguna. maxLines=2
  // ademas marca el estilo como puesto a mano (un smallText todo por omision
  // falla textStyleUnset y la lista volveria a poner bodyText).
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.valueText = screen.theme().smallText;
  // El texto de la fila arranca despues de la pestaña de 5 px del resalte.
  props.sidePadding = 16;
  // Interruptor de 36x20: marco redondeado y perilla llena a un lado, como el
  // del telefono. Encendido la lista rellena la via (fill negro + perilla
  // blanca): son 720 px de tinta, del orden de un icono de 24 px, que es lo
  // que la regla del negro macizo deja pasar, y es la unica forma de que se
  // vea encendido de un vistazo sin leer la palabra.
  props.toggleWidth = 36;
  props.toggleHeight = 20;
  props.toggleRadius = 10;
  props.toggleKnobRadius = 7;
  props.toggleKnobInset = 3;
  props.toggleBorderWidth = 1;
  // El resalte: NADA de pastilla tramada debajo del texto (es lo que hacia el
  // estilo LightPill del tema y dejaba la fila elegida como la menos legible
  // de la pantalla). Centro blanco, marco de 2 px y la pestaña negra de 5 px
  // de Selection.h llevada a la lista de fui como bitmap de marcador.
  fui::StyleSet rows = screen.theme().listRow.unset() ? fui::defaultListRowStyles() : screen.theme().listRow;
  rows.selected = rows.normal;
  rows.selected.background = fui::Paint::solid(fui::Color::White);
  rows.selected.foreground = fui::Paint::solid(fui::Color::Black);
  rows.selected.border = fui::Paint::solid(fui::Color::Black);
  rows.selected.borderWidth = 2;
  rows.focused = rows.selected;
  rows.active = rows.selected;
  props.rowStyles = rows;
  props.selectionMarker = fui::SelectionMarker::Bitmap;
  props.markerBitmap = fui::bitmapFromIcon(kSelectionTab);
  props.markerInset = 4;
  props.markerPaint = fui::Paint::solid(fui::Color::Black);
  syncTabListViewport(screen, props);

  // Una etiqueta que envuelve en dos renglones hace crecer SU fila, así que en
  // la pantalla entran menos filas que las que estima la cuenta de alto fijo
  // (listVisibleRows). Sin avisarle eso al nav, list() cortaba antes de dibujar
  // la última fila y el ítem de abajo parecía tapado por la barra de botones:
  // nunca se dibujaba. Con props.nav, list() informa lo que dibujó de verdad
  // (ListNav::onListRendered), corrige el viewport y pide otra pasada; render()
  // la hace. El ring guarda la posición 0 para la barra de pestañas, así que
  // durante el build el nav lleva el índice de fila que la corrección espera.
  auto& listNav = activeNav();
  const int ring = listNav.selected;
  listNav.selected = ring - 1;       // -1 = pestañas enfocadas, 0..N-1 = filas
  listNav.followPending = ring > 0;  // solo hay que perseguir una fila seleccionada
  props.nav = &listNav;
  screen.list(props);
  listNav.selected = ring;
  listNav.followPending = false;
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  renderUi();
  // Filas que envuelven: list() avisó cuántas entraron de verdad y el nav movió
  // el viewport para que la fila seleccionada se dibuje entera. Se repite el
  // dibujo con el viewport corregido (acotado: cada pasada acerca el tope a la
  // selección). Sin esto, la última fila de la lista quedaba invisible debajo
  // de la barra de botones.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                   CROSSPOINT_VERSION);
    renderUi();
  }

  const int ring = ringPos();
  // El nombre de la categoria siguiente no entra en un hueco de la barra
  // ("Controles" mide 93 px en UI_10 y manda a las CUATRO ayudas al modo
  // apilado): el verbo corto alcanza, la pestaña que se va a abrir ya se ve
  // resaltada arriba.
  const char* confirmLabel = tr(STR_SELECT);
  if (ring > 0) {
    const SettingInfo& row = (*currentSettings)[ring - 1];
    // Abrir otra pantalla o elegir un valor = "Selecc."; cambiar el ajuste ahi
    // mismo = "Editar".
    const bool opensScreen = row.type == SettingType::ACTION || row.nameId == StrId::STR_TIME_TO_SLEEP;
    confirmLabel = opensScreen ? tr(STR_SELECT) : tr(STR_TOGGLE);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
