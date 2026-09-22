#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  // REV-088: `hideUsbDrive` saca la fila del modo memoria USB. Se usa cuando se
  // llega desde **Ajustes -> Archivos**, que YA tiene su propia fila de USB
  // arriba: repetirla ahí sería ofrecer lo mismo dos veces en la misma pantalla
  // de la que se acaba de salir. Las demás placas llegan por la home clásica y
  // la siguen viendo.
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        bool hideUsbDrive = false);

#if FREEINK_CAP_USB_MSC
  static constexpr int MENU_ITEM_COUNT = 4;
#else
  static constexpr int MENU_ITEM_COUNT = 3;
#endif

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Row storage: entirely static (label/subtitle/icon never change), so it's
  // built once in the constructor instead of every buildScreen() call, into
  // fixed-capacity storage that avoids any heap allocation for the row list.
  freeink::ui::ListItem rowItems_[MENU_ITEM_COUNT]{};
  // Los modos van EN PARALELO a las filas y no se indexan sobre la tabla
  // estática: con el USB escondido los índices se corren, y leer `menuModes[i]`
  // con el índice de la fila abriría el modo de al lado.
  NetworkMode rowModes_[MENU_ITEM_COUNT]{};
  int rowCount_ = MENU_ITEM_COUNT;
};
