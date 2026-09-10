#pragma once

#include <ctime>
#include <string>
#include <vector>

#include <Icon.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Reminders and task lists from the hub cache (HubStore): a section list
// (Reminders, then each list with its pending count), and inside a section
// the items. OK ticks an item: it goes locally and POST /api/hub/done is sent
// or queued for the next sync, so it works without WiFi. Back goes up a level.
//
// Los recordatorios muestran debajo del título CUÁNDO vuelven a sonar (el
// `repeatText` que arma el servidor: "De lunes a viernes", "Cada 2 semanas") y
// con OK se abre la pantalla de edición: hora, fecha y repetición se eligen con
// la palanca, se ve el texto que va a quedar ANTES de guardar, y al guardar sale
// un POST /api/hub/reminder por la cola offline. El título se cambia por voz
// (el aparato no tiene teclado).
//
// El calendario y los viajes NO están acá: se fueron al mosaico "Mi día" del
// hub (CalendarActivity), que es donde va todo lo que tiene fecha junto con las
// sugerencias del día. Este mosaico es solo mensajes, recordatorios y listas.
class AgendaActivity final : public Activity {
 public:
  explicit AgendaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Agenda", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Level { SECTIONS, ITEMS, EDIT };
  Level level = SECTIONS;
  enum Kind { REMINDERS, LIST };
  struct Section {
    Kind kind;
    int listIndex;  // into HUB_STORE.lists when kind == LIST
  };
  std::vector<Section> sections;
  ButtonNavigator buttonNavigator;
  int sectionIndex = 0;
  int itemIndex = 0;
  int itemsPerPage = 1;
  OptionPopup menu;                   // Move / Date / Delete, then the sub-choice
  enum MenuStep { NONE, MAIN, MOVE, DATE };
  MenuStep menuStep = NONE;
  std::vector<std::string> menuOptions;
  int menuItemId = 0;

  // --- Edición de un recordatorio -----------------------------------------
  // Repeticiones que se ofrecen, en el mismo orden que se recorren con la
  // palanca. `weekly` lleva día de la semana y `weeks` lleva cada cuántas.
  enum RepeatKind { REP_ONCE, REP_DAILY, REP_WEEKDAYS, REP_WEEKLY, REP_WEEKS, REP_MONTHLY, REP_YEARLY, REP_COUNT };
  enum EditRow { ROW_TITLE, ROW_DATE, ROW_TIME, ROW_REPEAT, ROW_SAVE, ROW_DONE, ROW_DELETE, EDIT_ROWS };
  // Campo que se está cambiando con la palanca (F_NONE = moviéndose entre filas).
  enum EditField { F_NONE, F_DAY, F_MONTH, F_YEAR, F_HOUR, F_MINUTE, F_REPEAT, F_WEEKDAY, F_INTERVAL };
  struct EditState {
    int id = 0;
    std::string title;
    int year = 2026;
    int month = 1;
    int day = 1;
    int hour = -1;  // < 0 = sin hora
    int minute = 0;
    int repeatKind = REP_ONCE;
    int weekday = 0;  // 0 = lunes
    int interval = 2;
  };
  EditState edit;
  EditState editBackup;  // lo que había al entrar al campo: Atrás lo devuelve
  int editRow = ROW_TITLE;
  EditField editField = F_NONE;

  void rebuildSections();
  int sectionCount() const { return static_cast<int>(sections.size()); }
  int itemCount() const;
  const Section& current() const { return sections[sectionIndex]; }
  const freeink::Icon* sectionIcon(int index) const;
  std::string sectionTitle(int index) const;
  int sectionItemCount(int index) const;
  // Lo que hay adentro de la sección, en una línea: es lo que convierte la
  // lista de secciones en la pantalla que explica qué hay en cada una.
  std::string sectionPreview(int index) const;
  std::string itemText(int index, std::string& detail) const;
  // Las tres secciones como filas de dos renglones (vista previa y cuenta).
  void renderSections(int x, int top, int w, int bottom);
  // La lista de una sección, con la casilla de "OK lo tilda" en cada fila.
  void renderItems(int x, int top, int w, int bottom, int pagerY);
  // Barra de pestañas: en cuál estamos y cuánto hay en las otras. Devuelve la
  // y donde arrancan las filas.
  int drawTabs(int x, int y, int w);
  void tickCurrent();
  void openItemMenu();
  void onMenuPick(int index);
  void sendEdit(const char* action, const char* list, const char* dueDate);
  void openSection();

  void openEditor();
  void editFieldStep(int delta);
  void confirmEditRow();
  void saveEditedReminder();
  void deleteEditedReminder();
  std::string repeatLabel(bool marking) const;
  std::string dateLabel(bool marking) const;
  std::string timeLabel(bool marking) const;
  const char* repeatCode() const;
  void renderEditor();
};
