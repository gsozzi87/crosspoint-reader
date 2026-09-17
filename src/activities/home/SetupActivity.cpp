#include "SetupActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "HubStore.h"
#include "Memory.h"
#include "WifiCredentialStore.h"
#include "activities/ListStyle.h"
#include "activities/home/HubLocationActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/DevicePairActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Los idiomas del producto, en el orden en que se ofrecen. El chino quedó
// descartado y el resto del enum de CrossPoint no tiene traducción nuestra.
constexpr Language IDIOMAS[] = {Language::ES, Language::EN, Language::PT, Language::P2,
                                Language::FR, Language::DE, Language::RU};
constexpr int IDIOMAS_COUNT = static_cast<int>(sizeof(IDIOMAS) / sizeof(IDIOMAS[0]));

constexpr int VISIBLE_ROWS = 6;

}  // namespace

bool SetupActivity::pending() {
  if (!BoardConfig::isWS397()) return false;
  if (HUB_STORE.setupDone) return false;
  // Ya empezó: se sigue donde quedó. Va ANTES de las otras dos preguntas
  // porque después del paso del WiFi el aparato deja de parecer nuevo y el
  // asistente no volvería nunca.
  if (HUB_STORE.setupStep > 0) return true;
  // Un aparato que ya se venía usando NO tiene que ver el asistente al
  // actualizarse: si hay una red cargada o alguna vez sincronizó, está en uso.
  //
  // Lo del WiFi hay que CARGARLO primero: nadie carga `WIFI_STORE` en el
  // arranque (lo hacen las dos Activities de red cuando les toca), así que esta
  // pregunta se contestaba sobre un store vacío y daba 0 siempre. La guardia
  // existía y no guardaba nada.
  if (HUB_STORE.hasSynced()) return false;
  const bool wifiExiste = Storage.exists(WifiCredentialStore::getFilePath());
  const bool wifiSeLeyo = wifiExiste && WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() > 0) return false;

  // Y LAS DOS PREGUNTAS DE ARRIBA SE CONTESTAN CON LO QUE SE PUDO LEER, que no
  // es lo mismo que con lo que hay. Un JSON que EXISTE PERO NO PARSEA (una
  // tarjeta con la FAT dañada: pasó, le sacaron la batería en caliente) se lee
  // como store vacío, o sea "nunca sincronizó" y "no hay redes", y el aparato se
  // creía recién salido de la caja: asistente de primeros pasos y token nuevo.
  //
  // La pregunta es "existe y NO se pudo leer", no "existe" a secas. Con "existe"
  // a secas el asistente moría para siempre en un aparato REALMENTE nuevo:
  // apagar con PWR en la pantalla de idioma pasa por `powerOffNow()`, que hace
  // `HUB_STORE.saveToFile()` y crea hub.json sin que haya habido vida ninguna.
  if (wifiExiste && !wifiSeLeyo) return false;
  if (Storage.exists(HubStore::getFilePath()) && !HUB_STORE.loadFromFile()) return false;
  return true;
}

void SetupActivity::onEnter() {
  Activity::onEnter();
  // El idioma que ya tenga puesto arranca elegido, así el que vuelve a pasar
  // por acá no tiene que buscarlo.
  for (int i = 0; i < IDIOMAS_COUNT; ++i) {
    if (static_cast<uint8_t>(IDIOMAS[i]) == SETTINGS.language) {
      langIndex = i;
      break;
    }
  }
  scroll = std::max(0, std::min(langIndex - VISIBLE_ROWS + 1, IDIOMAS_COUNT - VISIBLE_ROWS));
  // Se retoma donde quedó: tres de los pasos reinician el aparato en silencio
  // al soltar la red.
  if (HUB_STORE.setupStep <= static_cast<uint8_t>(GESTURES)) {
    step = static_cast<Step>(HUB_STORE.setupStep);
  }
  requestUpdate();
}

void SetupActivity::advance() {
  switch (step) {
    case LANGUAGE:
      step = WIFI;
      break;
    case WIFI:
      step = PAIR;
      break;
    case PAIR:
      step = PLACE;
      break;
    case PLACE:
      step = GESTURES;
      break;
    case GESTURES:
      finishSetup();
      return;
  }
  HUB_STORE.setupStep = static_cast<uint8_t>(step);
  HUB_STORE.saveToFile();
  requestUpdate();
}

void SetupActivity::finishSetup() {
  HUB_STORE.setupDone = true;
  HUB_STORE.setupStep = 0;
  HUB_STORE.saveToFile();
  onGoHome();
}

void SetupActivity::openStepActivity() {
  // Cada paso es una pantalla que ya existe y que se usa también desde
  // Ajustes. Al volver, el asistente sigue en el paso siguiente pase lo que
  // pase: si no se pudo, se puede repetir desde Ajustes.
  waiting = true;
  // El paso guardado es el SIGUIENTE, no éste: si la pantalla que se abre
  // reinicia el aparato en silencio al soltar la red (Vincular y el Clima lo
  // hacen), al volver el asistente sigue de largo en vez de repetir el paso
  // que la persona acaba de hacer.
  if (step < GESTURES) {
    HUB_STORE.setupStep = static_cast<uint8_t>(step + 1);
    HUB_STORE.saveToFile();
  }
  auto seguir = [this](const ActivityResult&) {
    waiting = false;
    advance();
  };
  switch (step) {
    case WIFI:
      startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput), seguir);
      break;
    case PAIR:
      startActivityForResult(makeUniqueNoThrow<DevicePairActivity>(renderer, mappedInput), seguir);
      break;
    case PLACE:
      startActivityForResult(makeUniqueNoThrow<HubLocationActivity>(renderer, mappedInput), seguir);
      break;
    default:
      waiting = false;
      advance();
      break;
  }
}

void SetupActivity::loop() {
  if (waiting) return;

  if (step == LANGUAGE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) && langIndex > 0) {
      --langIndex;
      if (langIndex < scroll) scroll = langIndex;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) && langIndex < IDIOMAS_COUNT - 1) {
      ++langIndex;
      if (langIndex >= scroll + VISIBLE_ROWS) scroll = langIndex - VISIBLE_ROWS + 1;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      SETTINGS.language = static_cast<uint8_t>(IDIOMAS[langIndex]);
      I18N.setLanguage(IDIOMAS[langIndex]);
      SETTINGS.saveToFile();
      advance();
    }
    return;
  }

  if (step == GESTURES) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishSetup();
    }
    return;
  }

  // WiFi, vincular y clima: OK lo hace ahora, ABAJO (o Atrás) lo saltea.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openStepActivity();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    advance();
  }
}

void SetupActivity::renderLanguage() const {
  const int w = renderer.getScreenWidth();
  const int x = listui::SIDE;
  const int rowW = w - 2 * listui::SIDE;
  int y = listui::contentTop();

  // El encabezado va en los dos idiomas más probables y sin traducir: acá
  // todavía no se sabe cuál entiende el que lo está mirando.
  renderer.drawText(listui::sectionFont(), x, y, "Idioma  ·  Language");
  y += renderer.getLineHeight(listui::sectionFont()) + listui::GAP;

  for (int i = scroll; i < IDIOMAS_COUNT && i < scroll + VISIBLE_ROWS; ++i) {
    listui::row(renderer, x, y, rowW, listui::ROW1_H,
                {.title = LANGUAGE_NAMES[static_cast<uint8_t>(IDIOMAS[i])], .selected = i == langIndex});
    y += listui::ROW1_H;
  }
}

void SetupActivity::renderCard(const char* title, const char* body) const {
  const int w = renderer.getScreenWidth();
  const int x = listui::SIDE;
  const int maxW = w - 2 * listui::SIDE;
  int y = listui::contentTop() + listui::GAP * 2;

  renderer.drawText(listui::sectionFont(), x, y, title, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(listui::sectionFont()) + listui::GAP;
  listui::rule(renderer, x, y, maxW);
  y += listui::GAP * 2;

  // El cuerpo se parte a mano: una sola frase larga por paso, medida contra el
  // ancho real de la caja.
  const int line = renderer.getLineHeight(listui::titleFont());
  // Debajo de esto están los botones: el cuerpo se corta antes de pisarlos.
  const int tope = renderer.getScreenHeight() - line - listui::SIDE * 2;
  std::string resto = body;
  while (!resto.empty() && y <= tope) {
    size_t corte = resto.size();
    while (corte > 1 && renderer.getTextWidth(listui::titleFont(), resto.substr(0, corte).c_str()) > maxW) {
      // Un espacio en el 0 no sirve de corte (dejaría una línea vacía y el
      // resto sin avanzar): en ese caso se retrocede un carácter.
      const size_t espacio = resto.rfind(' ', corte - 1);
      if (espacio == std::string::npos || espacio == 0) {
        // Retroceder de a un carácter, no de a un byte: cortar en la mitad de
        // una letra de dos bytes (ruso, acentos) dibuja un glifo roto.
        --corte;
        while (corte > 1 && (static_cast<unsigned char>(resto[corte]) & 0xC0) == 0x80) --corte;
      } else {
        corte = espacio;
      }
    }
    renderer.drawText(listui::titleFont(), x, y, resto.substr(0, corte).c_str());
    y += line;
    resto = corte < resto.size() ? resto.substr(corte + (resto[corte] == ' ' ? 1 : 0)) : std::string();
  }
}

void SetupActivity::renderGestures() const {
  const int w = renderer.getScreenWidth();
  const int x = listui::SIDE;
  const int rowW = w - 2 * listui::SIDE;
  int y = listui::contentTop() + listui::GAP;

  renderer.drawText(listui::sectionFont(), x, y, tr(STR_SETUP_GESTURES_TITLE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(listui::sectionFont()) + listui::GAP;
  listui::rule(renderer, x, y, rowW);
  y += listui::GAP * 2;

  const struct {
    StrId gesto;
    StrId hace;
  } filas[] = {
      {StrId::STR_SETUP_GESTURE_VOICE, StrId::STR_SETUP_GESTURE_VOICE_DOES},
      {StrId::STR_SETUP_GESTURE_SYNC, StrId::STR_SETUP_GESTURE_SYNC_DOES},
      {StrId::STR_SETUP_GESTURE_PWR, StrId::STR_SETUP_GESTURE_PWR_DOES},
  };
  for (const auto& f : filas) {
    listui::row(renderer, x, y, rowW, listui::ROW2_H,
                {.title = I18N.get(f.gesto), .detail = I18N.get(f.hace), .bold = true});
    y += listui::ROW2_H;
  }
}

void SetupActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_SETUP_TITLE));

  // Los pasos de tarjeta no tienen lista, así que la palanca no navega nada:
  // ahí Atrás es "saltar" y se dice, en vez de dejar dos botones mudos.
  const char* btnBack = tr(STR_SETUP_SKIP);
  const char* btnOk = tr(STR_SETUP_DO_IT);
  const char* btnUp = "";
  const char* btnDown = "";

  switch (step) {
    case LANGUAGE:
      renderLanguage();
      btnBack = "";
      btnOk = tr(STR_SELECT);
      btnUp = tr(STR_DIR_UP);
      btnDown = tr(STR_DIR_DOWN);
      break;
    case WIFI:
      renderCard(tr(STR_SETUP_WIFI_TITLE), tr(STR_SETUP_WIFI_BODY));
      break;
    case PAIR:
      renderCard(tr(STR_SETUP_PAIR_TITLE), tr(STR_SETUP_PAIR_BODY));
      break;
    case PLACE:
      renderCard(tr(STR_SETUP_PLACE_TITLE), tr(STR_SETUP_PLACE_BODY));
      break;
    case GESTURES:
      renderGestures();
      btnBack = "";
      btnOk = tr(STR_SETUP_READY);
      break;
  }

  const auto labels = mappedInput.mapLabels(btnBack, btnOk, btnUp, btnDown);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
