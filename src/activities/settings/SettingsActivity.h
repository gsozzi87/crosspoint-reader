#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  // REV-088: la ayuda de la pestaña Archivos — qué carpeta es para qué. Abre el
  // visor de siempre; no hay nada que configurar.
  CardFolders,
  RemapFrontButtons,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
  KeyboardLayouts,
  AudioTest,
  ServerTest,
  HubSync,
  HubLocation,
  DownloadAssets,  // ws397: el paquete de contenido (dibujos, sonidos, Biblia)
  UsbDrive,        // ws397: la tarjeta como disco en la computadora, por cable
  DevicePair,      // ws397: vincular el aparato con una cuenta de la web
  Motion,          // ws397: gestos del IMU, valores en vivo y calibración de ejes
  Memory,          // ws397: stack declarado contra usado por tarea, heap y reposo
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  // El valor NO sale nunca por `GET /api/settings`: se puede escribir, no leer.
  // `obfuscated` es otra cosa (cómo se guarda en la tarjeta) y no alcanza: el
  // servidor web del aparato vive sobre un punto de acceso ABIERTO mientras se
  // carga la clave del WiFi desde el teléfono, y ese endpoint no pide credencial.
  bool secret = false;
  bool inTextSettings = false;  // Surfaced in the Text Settings screen; hidden from the flat Reader list
  // Se dibuja como interruptor aunque no sea SettingType::TOGGLE: un ENUM de
  // dos valores que en realidad es un si/no (los gestos del IMU). Los TOGGLE
  // de verdad no necesitan la marca.
  bool switchStyle = false;
  // REV-088: una fila que SÓLO informa (el espacio de la tarjeta). Es un STRING
  // con getter y sin setter, y hace falta la marca porque los otros STRING que
  // hay son la URL y el token del servidor: `settingValueText()` devuelve ""
  // para todos ellos a propósito, y empezar a pintar el valor de golpe sacaría
  // el token a la pantalla. Se pinta sólo lo que pide que se pinte.
  bool readOnlyText = false;

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withSecret() {
    secret = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  SettingInfo& withSwitch() {
    switchStyle = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  /// Fila de sólo lectura: etiqueta a la izquierda, lo que devuelva el getter a
  /// la derecha. No tiene setter, así que OK encima no hace nada, y no lleva
  /// `key`: nunca sale por `GET /api/settings`.
  static SettingInfo Info(StrId nameId, std::function<std::string()> getter) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.readOnlyText = true;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  // Igual que Value, pero para algo que NO vive en CrossPointSettings (el
  // volumen del aparato vive en HubStore, junto al resto de los ajustes del
  // hub). Sin esto no había forma de tocar el volumen desde Ajustes, que es
  // donde el usuario lo fue a buscar.
  static SettingInfo DynamicValue(StrId nameId, const ValueRange valueRange, std::function<uint8_t()> getter,
                                  std::function<void(uint8_t)> setter, const char* key = nullptr,
                                  StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valueRange = valueRange;
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public UiTabListActivity {
  int selectedCategoryIndex = 0;  // Currently selected category
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  // REV-088: la pestaña Archivos existe SÓLO en la ws397, así que el juego de
  // pestañas es de la placa y no una constante. Antes había un `categoryCount`
  // fijo en 4 y DOS `switch` con los mismos cuatro casos copiados; con una
  // pestaña condicional eso se habría separado solo, que es exactamente cómo se
  // separaron las rutas protegidas en 1.5.91. Ahora hay UNA tabla.
  std::vector<SettingInfo> filesSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue) for *currentSettings, rebuilt only when
  // the active category or a category's setting list changes
  // (rebuildRowItems(), called from selectCategory()/rebuildSettingsLists())
  // — not on every repaint. rowValues_ holds the live per-row value text,
  // refreshed every buildScreen() call by assigning into the existing
  // strings (no vector growth).
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  // Las pestañas de ESTA placa, en orden, y la lista que abre cada una. Se
  // arma una sola vez (`buildTabTable()` desde el constructor): depende del
  // modelo, que no cambia en caliente. Los punteros apuntan a los vectores
  // miembro, que no se mueven; `clear()`/`push_back()` sobre ellos no los
  // invalida.
  struct TabDef {
    StrId name;
    std::vector<SettingInfo> SettingsActivity::* list;
  };
  std::vector<TabDef> tabs_;
  void buildTabTable();
  void pointCurrentSettings();

  // --- UiTabListActivity contract ---
  int listCount() const override { return settingsCount; }
  int tabCount() const override { return static_cast<int>(tabs_.size()); }
  int activeTab() const override { return selectedCategoryIndex; }
  const char* tabLabel(const int index) const override {
    if (index < 0 || index >= tabCount()) return "";
    return I18N.get(tabs_[index].name);
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  bool handleCustomInput() override;

  // Galon (U+203A) de las filas que abren otra pantalla.
  static const char* const chevronGlyph;
  static std::string settingValueText(const SettingInfo& setting);
  void selectCategory(int categoryIndex);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
};
