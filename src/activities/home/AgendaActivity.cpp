#include "AgendaActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>

#include <algorithm>
#include <ctime>

#include "CalendarActivity.h"
#include "HubStore.h"
#include "MappedInputManager.h"
#include "VoiceActivity.h"
#include "activities/ListStyle.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "components/icons/hubWidgetIcons.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "AGENDA";
constexpr unsigned long MENU_HOLD_MS = 1200;
constexpr int PAGER_H = 24;   // franja del paginador, debajo de las filas
constexpr int TABS_H = 40;    // barra de pestañas de la vista de ítems
constexpr int TAB_GAP = 24;   // aire entre una pestaña y la siguiente
constexpr int TAB_COUNT_GAP = 8;

std::string dateOffset(int days) {
  time_t now = 0;
  if (!halClock.getEpochUtc(now)) return "";
  now += static_cast<time_t>(days) * 86400;
  struct tm t;
  gmtime_r(&now, &t);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

// "[14]" cuando ese es el pedazo que está cambiando la palanca: en una pantalla
// en blanco y negro es la forma más clara de decir dónde está el foco.
std::string mark(const std::string& text, const bool marking) { return marking ? "[" + text + "]" : text; }

std::string twoDigits(const int value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d", value);
  return buf;
}

// Reemplaza el %d de una frase traducida por un texto ya armado (que puede
// venir entre corchetes), sin pasar por printf.
std::string formatCount(const char* fmt, const std::string& value) {
  std::string out = fmt ? fmt : "";
  const size_t at = out.find("%d");
  if (at == std::string::npos) return out + " " + value;
  out.replace(at, 2, value);
  return out;
}

// Cuándo vuelve a sonar, en palabras. Lo normal es que la frase la mande armada
// el servidor (`repeatText`); esto es el respaldo con el código suelto. Si no
// viene ninguno de los dos se devuelve vacío: mejor no decir nada que decir
// "una sola vez" sobre algo que a lo mejor repite.
std::string repeatFromCode(const std::string& code, const int weekday, const int interval) {
  if (code == "daily") return tr(STR_REP_DAILY);
  if (code == "weekdays") return tr(STR_REP_WEEKDAYS);
  if (code == "weekly") {
    return std::string(tr(STR_REP_WEEKLY)) + ": " +
           CalendarActivity::weekdayName(weekday >= 0 && weekday <= 6 ? weekday : 0);
  }
  if (code == "weeks") return formatCount(tr(STR_REP_WEEKS_FORMAT), std::to_string(interval >= 2 ? interval : 2));
  if (code == "monthly") return tr(STR_REP_MONTHLY);
  if (code == "yearly") return tr(STR_REP_YEARLY);
  if (code == "once") return tr(STR_REP_ONCE);
  return "";
}
}  // namespace

void AgendaActivity::onEnter() {
  Activity::onEnter();
  level = SECTIONS;
  sectionIndex = 0;
  itemIndex = 0;
  rebuildSections();
  applyFocus();
  requestUpdate();
}

// Lo que se acaba de dictar se abre donde vive, no en un cartel. Un
// recordatorio entra directo al editor (hora, fecha, repetición) porque es lo
// único que hay que revisar antes de que suene; un item de lista alcanza con
// dejarlo marcado en su seccion.
bool AgendaActivity::applyFocus() {
  if (focus_ == Focus::None || focusId_ <= 0) return false;
  const Focus what = focus_;
  focus_ = Focus::None;  // solo al entrar: despues manda la navegacion normal
  if (what == Focus::Reminder) {
    for (size_t i = 0; i < HUB_STORE.reminders.size(); i++) {
      if (HUB_STORE.reminders[i].id != focusId_) continue;
      for (int sIdx = 0; sIdx < sectionCount(); sIdx++) {
        if (sections[sIdx].kind != REMINDERS) continue;
        sectionIndex = sIdx;
        itemIndex = static_cast<int>(i);
        level = ITEMS;
        openEditor();
        return true;
      }
      return false;
    }
    return false;
  }
  for (int sIdx = 0; sIdx < sectionCount(); sIdx++) {
    if (sections[sIdx].kind != LIST) continue;
    const HubStore::List& l = HUB_STORE.lists[sections[sIdx].listIndex];
    for (size_t i = 0; i < l.items.size(); i++) {
      if (l.items[i].id != focusId_) continue;
      sectionIndex = sIdx;
      itemIndex = static_cast<int>(i);
      level = ITEMS;
      return true;
    }
  }
  return false;
}

// Recordatorios primero y después las listas. En 1.5.44 se sacaron los
// mensajes ("quítalos del sistema, no me gusta, se me hace algo irrelevante") y
// las listas quedaron sin categorías: el servidor manda sólo dos, compras y
// tareas. El calendario y los viajes viven en el mosaico "Mi día" del hub.
void AgendaActivity::rebuildSections() {
  sections.clear();
  sections.push_back({REMINDERS, -1});
  for (int i = 0; i < static_cast<int>(HUB_STORE.lists.size()); ++i) sections.push_back({LIST, i});
  if (sectionIndex >= sectionCount()) sectionIndex = sectionCount() - 1;
}

int AgendaActivity::itemCount() const {
  switch (current().kind) {
    case REMINDERS: return static_cast<int>(HUB_STORE.reminders.size());
    case LIST: return static_cast<int>(HUB_STORE.lists[current().listIndex].items.size());
  }
  return 0;
}

int AgendaActivity::sectionItemCount(const int index) const {
  switch (sections[index].kind) {
    case REMINDERS: return static_cast<int>(HUB_STORE.reminders.size());
    case LIST: return static_cast<int>(HUB_STORE.lists[sections[index].listIndex].items.size());
  }
  return 0;
}

std::string AgendaActivity::sectionTitle(const int index) const {
  switch (sections[index].kind) {
    case REMINDERS: return tr(STR_HUB_REMINDERS);
    case LIST: return HUB_STORE.lists[sections[index].listIndex].name;
  }
  return "";
}

// La primera línea de lo que hay adentro. En Recordatorios, el próximo con su
// cuándo; en una lista, los primeros ítems separados por comas.
std::string AgendaActivity::sectionPreview(const int index) const {
  switch (sections[index].kind) {
    case REMINDERS: {
      if (HUB_STORE.reminders.empty()) break;
      const HubStore::Reminder& r = HUB_STORE.reminders[0];
      return r.when.empty() ? r.title : r.title + " · " + r.when;
    }
    case LIST: {
      const std::vector<HubStore::ListItem>& items = HUB_STORE.lists[sections[index].listIndex].items;
      if (items.empty()) break;
      std::string out;
      for (int i = 0; i < static_cast<int>(items.size()) && i < 4; ++i) {
        out += out.empty() ? items[i].text : ", " + items[i].text;
      }
      if (items.size() > 4) out += "...";
      return out;
    }
  }
  return tr(STR_AGENDA_EMPTY);
}

std::string AgendaActivity::itemText(const int index, std::string& detail) const {
  detail.clear();
  switch (current().kind) {
    case REMINDERS: {
      const HubStore::Reminder& r = HUB_STORE.reminders[index];
      detail = r.when;
      return r.title;
    }
    case LIST: return HUB_STORE.lists[current().listIndex].items[index].text;
  }
  return "";
}

// Local removal first (the screen must answer right away), then the server:
// delivered now if the network is up, queued on the SD otherwise and replayed
// by the next hub sync.
void AgendaActivity::tickCurrent() {
  if (itemCount() == 0) return;
  int id = 0;
  const char* kind = "item";
  // dueAt de la ocurrencia que se esta tildando: viaja en el POST como `at`
  // para que un reintento no le coma otro ciclo a un recordatorio que repite.
  time_t at = 0;
  switch (current().kind) {
    case REMINDERS: {
      kind = "reminder";
      id = HUB_STORE.reminders[itemIndex].id;
      at = HUB_STORE.reminders[itemIndex].dueAt;
      // Los que repiten no se borran de la cache: se les corre la fecha, para
      // que la alarma siga armada aunque no haya WiFi (el servidor hace lo
      // mismo con advanceRepeat() y manda la version buena al sincronizar).
      time_t now = 0;
      halClock.getEpochUtc(now);
      HUB_STORE.completeReminder(id, now);
      break;
    }
    case LIST:
      id = HUB_STORE.lists[current().listIndex].items[itemIndex].id;
      HUB_STORE.removeItem(id);
      break;
  }
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = kind;
    doc["id"] = id;
    if (at > 0) doc["at"] = static_cast<int64_t>(at);
    serializeJson(doc, body);
  }
  const ServerClient::Result r = SERVER_CLIENT.postOrQueue("/api/hub/done", body);
  LOG_INF(TAG, "done %s %d: %s", kind, id, ServerClient::resultName(r));
  if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
  // Tildar el último deja la sección vacía: en vez de una lista en blanco bajo
  // las pestañas se vuelve a las tres secciones, que es la pantalla que dice
  // qué hay en cada una.
  if (itemCount() == 0) level = SECTIONS;
  requestUpdate();
}

void AgendaActivity::sendEdit(const char* action, const char* list, const char* dueDate) {
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "item";
    doc["id"] = menuItemId;
    doc["action"] = action;
    if (list) doc["list"] = list;
    if (dueDate) doc["dueDate"] = dueDate;
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "edit %s %d: %s", action, menuItemId, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/edit", body)));
}

void AgendaActivity::openItemMenu() {
  if (level != ITEMS || current().kind != LIST || itemCount() == 0) return;  // lists only; the rest just ticks
  menuItemId = HUB_STORE.lists[current().listIndex].items[itemIndex].id;
  menuStep = MAIN;
  menuOptions = {tr(STR_AGENDA_MOVE), tr(STR_AGENDA_DATE), tr(STR_AGENDA_DELETE)};
  menu.show(StrId::STR_AGENDA_ITEM_MENU, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
  requestUpdate();
}

void AgendaActivity::onMenuPick(const int index) {
  if (menuStep == MAIN) {
    if (index == 0) {
      menuStep = MOVE;
      menuOptions.clear();
      for (const HubStore::List& l : HUB_STORE.lists) menuOptions.push_back(l.name);
      menu.show(StrId::STR_AGENDA_MOVE, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
    } else if (index == 1) {
      menuStep = DATE;
      menuOptions = {tr(STR_AGENDA_DATE_TODAY), tr(STR_AGENDA_DATE_TOMORROW), tr(STR_AGENDA_DATE_NEXT_WEEK),
                     tr(STR_AGENDA_DATE_NONE)};
      menu.show(StrId::STR_AGENDA_DATE, menuOptions, 0, [this](int idx) { onMenuPick(idx); });
    } else if (index == 2) {
      HUB_STORE.removeItem(menuItemId);
      HUB_STORE.saveToFile();
      sendEdit("delete", nullptr, nullptr);
      menuStep = NONE;
      if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
    } else {
      menuStep = NONE;
    }
  } else if (menuStep == MOVE) {
    if (index >= 0 && index < static_cast<int>(HUB_STORE.lists.size())) {
      // Al servidor va la CLAVE de la lista, no el nombre traducido.
      const std::string target = HUB_STORE.lists[index].key;
      HUB_STORE.moveItem(menuItemId, target);
      HUB_STORE.saveToFile();
      sendEdit("move", target.c_str(), nullptr);
      if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
    }
    menuStep = NONE;
  } else if (menuStep == DATE) {
    if (index == 0) sendEdit("date", nullptr, dateOffset(0).c_str());
    else if (index == 1) sendEdit("date", nullptr, dateOffset(1).c_str());
    else if (index == 2) sendEdit("date", nullptr, dateOffset(7).c_str());
    else if (index == 3) sendEdit("date", nullptr, "");
    menuStep = NONE;
  }
  requestUpdate();
}

// OK sobre una sección: abre su lista de ítems. Una sección vacía no se abre
// (no hay nada que mostrar y la fila ya dice "Nada pendiente"): se queda en las
// secciones, que es lo que explica qué hay adentro de cada una.
void AgendaActivity::openSection() {
  if (sectionItemCount(sectionIndex) == 0) return;
  level = ITEMS;
  itemIndex = 0;
  requestUpdate();
}

// --------------------------------------------------------------------------
// Edición de un recordatorio
// --------------------------------------------------------------------------

const char* AgendaActivity::repeatCode() const {
  switch (edit.repeatKind) {
    case REP_DAILY: return "daily";
    case REP_WEEKDAYS: return "weekdays";
    case REP_WEEKLY: return "weekly";
    case REP_WEEKS: return "weeks";
    case REP_MONTHLY: return "monthly";
    case REP_YEARLY: return "yearly";
    default: return "once";
  }
}

std::string AgendaActivity::repeatLabel(const bool marking) const {
  const bool markKind = marking && editField == F_REPEAT;
  const bool markDow = marking && editField == F_WEEKDAY;
  const bool markN = marking && editField == F_INTERVAL;
  switch (edit.repeatKind) {
    case REP_DAILY: return mark(tr(STR_REP_DAILY), markKind);
    case REP_WEEKDAYS: return mark(tr(STR_REP_WEEKDAYS), markKind);
    case REP_WEEKLY:
      return mark(tr(STR_REP_WEEKLY), markKind) + ": " +
             mark(CalendarActivity::weekdayName(edit.weekday), markDow);
    case REP_WEEKS:
      return mark(formatCount(tr(STR_REP_WEEKS_FORMAT), mark(std::to_string(edit.interval), markN)), markKind);
    case REP_MONTHLY: return mark(tr(STR_REP_MONTHLY), markKind);
    case REP_YEARLY: return mark(tr(STR_REP_YEARLY), markKind);
    default: return mark(tr(STR_REP_ONCE), markKind);
  }
}

std::string AgendaActivity::dateLabel(const bool marking) const {
  const int dow = CalendarActivity::weekdayOfCivil(edit.year, edit.month, edit.day);
  return std::string(CalendarActivity::weekdayShort(dow)) + " " +
         mark(twoDigits(edit.day), marking && editField == F_DAY) + "/" +
         mark(twoDigits(edit.month), marking && editField == F_MONTH) + "/" +
         mark(std::to_string(edit.year), marking && editField == F_YEAR);
}

std::string AgendaActivity::timeLabel(const bool marking) const {
  if (edit.hour < 0) return mark(tr(STR_REM_NO_TIME), marking && editField == F_HOUR);
  return mark(twoDigits(edit.hour), marking && editField == F_HOUR) + ":" +
         mark(twoDigits(edit.minute), marking && editField == F_MINUTE);
}

// Abre el recordatorio marcado: fecha y hora salen del `dueAt` (epoch del
// servidor) pasadas al huso del aparato, y la repetición del código que manda
// GET /api/hub. Sin nada de eso, arranca hoy y sin hora.
void AgendaActivity::openEditor() {
  if (current().kind != REMINDERS || itemIndex >= itemCount()) return;
  const HubStore::Reminder& r = HUB_STORE.reminders[itemIndex];
  edit = EditState();
  edit.id = r.id;
  edit.title = r.title;
  int y = 0, m = 0, d = 0, hh = 0, mi = 0;
  if (r.dueAt > 0) {
    CalendarActivity::localFromEpoch(r.dueAt, y, m, d, hh, mi);
    edit.year = y;
    edit.month = m;
    edit.day = d;
    edit.hour = hh;
    edit.minute = mi;
  } else if (CalendarActivity::localToday(y, m, d)) {
    edit.year = y;
    edit.month = m;
    edit.day = d;
  }
  if (r.repeat == "daily") edit.repeatKind = REP_DAILY;
  else if (r.repeat == "weekdays") edit.repeatKind = REP_WEEKDAYS;
  else if (r.repeat == "weekly") edit.repeatKind = REP_WEEKLY;
  else if (r.repeat == "weeks") edit.repeatKind = REP_WEEKS;
  else if (r.repeat == "monthly") edit.repeatKind = REP_MONTHLY;
  else if (r.repeat == "yearly") edit.repeatKind = REP_YEARLY;
  else edit.repeatKind = REP_ONCE;
  edit.weekday = r.weekday >= 0 && r.weekday <= 6
                     ? r.weekday
                     : CalendarActivity::weekdayOfCivil(edit.year, edit.month, edit.day);
  edit.interval = r.interval >= 2 ? r.interval : 2;
  editRow = ROW_DATE;  // lo primero que se suele cambiar
  editField = F_NONE;
  level = EDIT;
  requestUpdate();
}

// La palanca dentro de un campo. Cada campo se mueve solo por sus valores
// posibles y da la vuelta, así nunca hay que "pasarse" para volver.
void AgendaActivity::editFieldStep(const int delta) {
  switch (editField) {
    case F_DAY: {
      const int dim = CalendarActivity::daysInMonth(edit.year, edit.month);
      edit.day = ((edit.day - 1 + delta) % dim + dim) % dim + 1;
      break;
    }
    case F_MONTH:
      edit.month = ((edit.month - 1 + delta) % 12 + 12) % 12 + 1;
      edit.day = std::min(edit.day, CalendarActivity::daysInMonth(edit.year, edit.month));
      break;
    case F_YEAR:
      edit.year = std::min(std::max(edit.year + delta, 2024), 2099);
      edit.day = std::min(edit.day, CalendarActivity::daysInMonth(edit.year, edit.month));
      break;
    case F_HOUR:
      // -1 = sin hora: entra en la rueda antes de las 00, así se puede sacar la
      // hora sin otro menú.
      edit.hour = ((edit.hour + 1 + delta) % 25 + 25) % 25 - 1;
      break;
    case F_MINUTE:
      edit.minute = ((edit.minute / 5 + delta) % 12 + 12) % 12 * 5;
      break;
    case F_REPEAT:
      edit.repeatKind = ((edit.repeatKind + delta) % REP_COUNT + REP_COUNT) % REP_COUNT;
      break;
    case F_WEEKDAY:
      edit.weekday = ((edit.weekday + delta) % 7 + 7) % 7;
      break;
    case F_INTERVAL:
      edit.interval = ((edit.interval - 2 + delta) % 7 + 7) % 7 + 2;  // de 2 a 8 semanas
      break;
    case F_NONE:
      break;
  }
  requestUpdate();
}

void AgendaActivity::confirmEditRow() {
  if (editField != F_NONE) {
    // OK dentro de un campo pasa al siguiente pedazo y al final confirma.
    switch (editField) {
      case F_DAY: editField = F_MONTH; break;
      case F_MONTH: editField = F_YEAR; break;
      case F_HOUR: editField = edit.hour < 0 ? F_NONE : F_MINUTE; break;
      case F_REPEAT:
        editField = edit.repeatKind == REP_WEEKLY ? F_WEEKDAY : edit.repeatKind == REP_WEEKS ? F_INTERVAL : F_NONE;
        break;
      default: editField = F_NONE; break;
    }
    requestUpdate();
    return;
  }
  switch (editRow) {
    case ROW_TITLE:
      // El aparato no tiene teclado: el título se cambia hablando. Lo que ya se
      // tocó acá se guarda antes de irse, para no perderlo.
      saveEditedReminder();
      activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInput));
      return;
    case ROW_DATE:
      editBackup = edit;
      editField = F_DAY;
      break;
    case ROW_TIME:
      editBackup = edit;
      editField = F_HOUR;
      break;
    case ROW_REPEAT:
      editBackup = edit;
      editField = F_REPEAT;
      break;
    case ROW_SAVE:
      saveEditedReminder();
      level = ITEMS;
      break;
    case ROW_DONE:
      level = ITEMS;
      tickCurrent();
      return;
    case ROW_DELETE:
      deleteEditedReminder();
      return;
    default:
      break;
  }
  requestUpdate();
}

// POST /api/hub/reminder por la cola offline (igual que el resto): el aparato
// no espera a tener WiFi para dejar el cambio hecho.
void AgendaActivity::saveEditedReminder() {
  std::string body;
  {
    JsonDocument doc;
    doc["id"] = edit.id;
    doc["date"] = CalendarActivity::isoDate(edit.year, edit.month, edit.day);
    doc["time"] = edit.hour >= 0 ? twoDigits(edit.hour) + ":" + twoDigits(edit.minute) : std::string();
    doc["repeat"] = repeatCode();
    if (edit.repeatKind == REP_WEEKLY) doc["weekday"] = edit.weekday;
    if (edit.repeatKind == REP_WEEKS) doc["interval"] = edit.interval;
    serializeJson(doc, body);
  }
  // El idioma va en la URL: el servidor contesta con el texto de la repetición
  // ya traducido (acá se arma igual para no esperar a la próxima sincronización).
  const ServerClient::Result r =
      SERVER_CLIENT.postOrQueue("/api/hub/reminder?lang=" + std::string(uiLanguageCode()), body);
  LOG_INF(TAG, "reminder %d: %s", edit.id, ServerClient::resultName(r));

  // La caché queda al día sin esperar la próxima sincronización: el texto de la
  // repetición se arma acá con las mismas palabras que manda el servidor.
  for (HubStore::Reminder& rem : HUB_STORE.reminders) {
    if (rem.id != edit.id) continue;
    rem.dueAt = edit.hour >= 0
                    ? CalendarActivity::epochFromLocal(edit.year, edit.month, edit.day, edit.hour, edit.minute)
                    : 0;
    rem.repeat = repeatCode();
    rem.weekday = edit.repeatKind == REP_WEEKLY ? edit.weekday : -1;
    rem.interval = edit.repeatKind == REP_WEEKS ? edit.interval : 0;
    rem.repeatText = repeatLabel(false);
    rem.when = dateLabel(false) + (edit.hour >= 0 ? " " + timeLabel(false) : "");
    break;
  }
  if (!HUB_STORE.reminders.empty()) {
    HUB_STORE.reminderTitle = HUB_STORE.reminders[0].title;
    HUB_STORE.reminderWhen = HUB_STORE.reminders[0].when;
  }
  HUB_STORE.saveToFile();
}

void AgendaActivity::deleteEditedReminder() {
  const int id = edit.id;
  HUB_STORE.removeReminder(id);
  HUB_STORE.saveToFile();
  std::string body;
  {
    JsonDocument doc;
    doc["kind"] = "reminder";
    doc["id"] = id;
    doc["action"] = "delete";
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "delete reminder %d: %s", id, ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/hub/edit", body)));
  level = ITEMS;
  if (itemIndex >= itemCount() && itemIndex > 0) itemIndex--;
  requestUpdate();
}

void AgendaActivity::loop() {
  // La caché puede vaciarse por debajo (una sincronización, un tilde desde el
  // hub): sin sección que mostrar, la pantalla vuelve sola a las tres filas.
  if (level == ITEMS && itemCount() == 0) {
    level = SECTIONS;
    requestUpdate();
  }
  if (menuStep != NONE) {
    if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!menu.isActive() && menuStep != NONE) {
        // Back on a popup: a sub-menu returns to the main one, the main one closes.
        if (menuStep == MAIN) {
          menuStep = NONE;
        } else {
          menuStep = NONE;
          openItemMenu();
        }
        requestUpdate();
      }
    }
    return;
  }

  if (level == EDIT) {
    if (editField != F_NONE) {
      buttonNavigator.onNext([this] { editFieldStep(1); });
      buttonNavigator.onPrevious([this] { editFieldStep(-1); });
    } else {
      buttonNavigator.onNext([this] {
        editRow = ButtonNavigator::nextIndex(editRow, EDIT_ROWS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        editRow = ButtonNavigator::previousIndex(editRow, EDIT_ROWS);
        requestUpdate();
      });
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmEditRow();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Atrás siempre sale: primero del campo (dejándolo como estaba) y después
      // de la edición.
      if (editField != F_NONE) {
        edit = editBackup;
        editField = F_NONE;
      } else {
        level = ITEMS;
      }
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, MENU_HOLD_MS)) {
    openItemMenu();
    return;
  }
  const int count = level == SECTIONS ? sectionCount() : itemCount();
  int& index = level == SECTIONS ? sectionIndex : itemIndex;

  buttonNavigator.onNext([&] {
    if (count > 0) index = ButtonNavigator::nextIndex(index, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([&] {
    if (count > 0) index = ButtonNavigator::previousIndex(index, count);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (level == SECTIONS) {
      openSection();
    } else if (current().kind == REMINDERS) {
      openEditor();  // un recordatorio se abre para ver y cambiar cuándo suena
    } else {
      tickCurrent();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (level == ITEMS) {
      level = SECTIONS;
      requestUpdate();
    } else {
      finish();
    }
  }
}

// Pantalla de edición: una fila por cosa que se puede cambiar, con el valor que
// va a quedar a la derecha. Nada de teclados: la palanca recorre los valores.
void AgendaActivity::renderEditor() {
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  const int top = listui::contentTop();

  for (int row = 0; row < EDIT_ROWS; ++row) {
    const int y = top + row * listui::ROW1_H;
    const bool sel = row == editRow;
    const char* label = tr(STR_REM_TITLE_ROW);
    std::string value;
    switch (row) {
      case ROW_TITLE:
        value = edit.title;
        break;
      case ROW_DATE:
        label = tr(STR_REM_DATE);
        value = dateLabel(sel);
        break;
      case ROW_TIME:
        label = tr(STR_REM_TIME);
        value = timeLabel(sel);
        break;
      case ROW_REPEAT:
        label = tr(STR_REM_REPEAT);
        value = repeatLabel(sel);
        break;
      case ROW_SAVE: label = tr(STR_REM_SAVE); break;
      case ROW_DONE: label = tr(STR_REM_DONE_ROW); break;
      case ROW_DELETE: label = tr(STR_REM_DELETE_ROW); break;
      default: break;
    }
    listui::RowSpec spec;
    spec.title = label;
    spec.meta = value.empty() ? nullptr : value.c_str();
    spec.selected = sel;
    listui::row(renderer, x, y, w, listui::ROW1_H, spec);
  }

  const char* hint = editField != F_NONE ? tr(STR_REM_FIELD_HINT)
                     : editRow == ROW_TITLE ? tr(STR_REM_VOICE_HINT)
                                            : tr(STR_REM_ROW_HINT);
  listui::hint(renderer, listui::contentBottom(renderer) - listui::HINT_H, hint);
}

// Las tres secciones (Recordatorios, Compras, Tareas) con una vista previa de
// lo que hay adentro y la cuenta a la derecha. Es la pantalla de entrada y la
// que se muestra cuando la sección elegida se quedó sin nada: una lista en
// blanco bajo las pestañas no dice nada, esto sí.
// Campana, carrito y lista de tareas: las tres secciones se distinguen de un
// vistazo sin leer, que es lo que pide la maqueta.
const freeink::Icon* AgendaActivity::sectionIcon(const int index) const {
  if (sections[index].kind == REMINDERS) return &icon_hub_bell_24;
  // "Compras" es la clave canónica del store del servidor (server/src/store.ts),
  // no el nombre traducido: el carrito no depende del idioma.
  return HUB_STORE.lists[sections[index].listIndex].key == "Compras" ? &icon_hub_cart_24 : &icon_hub_tasks_24;
}

void AgendaActivity::renderSections(const int x, const int top, const int w, const int bottom) {
  const int count = sectionCount();
  itemsPerPage = std::max(1, (bottom - top) / listui::ROW2_H);
  const int first = count > 0 ? (sectionIndex / itemsPerPage) * itemsPerPage : 0;
  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * listui::ROW2_H;
    const std::string title = sectionTitle(i);
    const std::string preview = sectionPreview(i);
    const std::string countText = std::to_string(sectionItemCount(i));
    listui::RowSpec spec;
    spec.title = title.c_str();
    spec.icon = sectionIcon(i);
    spec.detail = preview.c_str();
    spec.meta = countText.c_str();
    spec.metaBox = true;
    spec.bold = true;
    spec.selected = i == sectionIndex;
    listui::row(renderer, x, y, w, listui::ROW2_H, spec);
  }
}

// La barra de pestañas de la vista de ítems: dice en cuál estamos y cuánto hay
// en las otras (la palanca mueve la selección, no la pestaña; a las otras se
// llega con Atrás). Si los nombres no entran, queda sola la activa.
int AgendaActivity::drawTabs(const int x, const int y, const int w) {
  const int count = sectionCount();
  int total = 0;
  for (int i = 0; i < count; ++i) {
    const std::string title = sectionTitle(i);
    const std::string n = std::to_string(sectionItemCount(i));
    total += renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD) + TAB_COUNT_GAP +
             renderer.getTextWidth(UI_10_FONT_ID, n.c_str());
  }
  total += TAB_GAP * (count - 1);
  const bool all = total <= w;

  const int textY = y + (TABS_H - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 2;
  int cx = x;
  for (int i = 0; i < count; ++i) {
    if (!all && i != sectionIndex) continue;
    const bool active = i == sectionIndex;
    const auto style = active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string title = sectionTitle(i);
    const std::string n = std::to_string(sectionItemCount(i));
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), style);
    const int nW = renderer.getTextWidth(UI_10_FONT_ID, n.c_str());
    renderer.drawText(UI_10_FONT_ID, cx, textY, title.c_str(), true, style);
    renderer.drawText(UI_10_FONT_ID, cx + titleW + TAB_COUNT_GAP, textY, n.c_str());
    if (active) renderer.fillRect(cx, y + TABS_H - 4, titleW, 3, true);
    cx += titleW + TAB_COUNT_GAP + nW + TAB_GAP;
  }
  listui::rule(renderer, x, y + TABS_H - 1, w);
  return y + TABS_H + listui::GAP;
}

// Los ítems de la sección. Los de una lista llevan casilla vacía a la izquierda:
// es lo que dice, sin manual, que OK los tilda. Los recordatorios NO la llevan,
// porque ahí OK abre el editor de cuándo suena: una casilla prometería un tilde
// que no va a pasar.
void AgendaActivity::renderItems(const int x, const int top, const int w, const int bottom, const int pagerY) {
  const bool reminders = current().kind == REMINDERS;
  const int rowH = reminders ? listui::ROW2_H : listui::ROW1_H;
  const int count = itemCount();
  itemsPerPage = std::max(1, (bottom - top) / rowH);
  const int page = count > 0 ? itemIndex / itemsPerPage : 0;
  const int first = page * itemsPerPage;

  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * rowH;
    std::string when;
    const std::string text = itemText(i, when);
    std::string repeat;
    if (reminders) {
      const HubStore::Reminder& r = HUB_STORE.reminders[i];
      repeat = r.repeatText.empty() ? repeatFromCode(r.repeat, r.weekday, r.interval) : r.repeatText;
    }
    listui::RowSpec spec;
    spec.title = text.c_str();
    spec.detail = repeat.empty() ? nullptr : repeat.c_str();
    spec.meta = when.empty() ? nullptr : when.c_str();
    spec.check = !reminders;
    spec.selected = i == itemIndex;
    listui::row(renderer, x, y, w, rowH, spec);
  }
  listui::pager(renderer, x, pagerY, w, page + 1, (count + itemsPerPage - 1) / itemsPerPage);
}

void AgendaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  // Sin nada adentro se muestran las secciones, aunque el nivel diga ITEMS: la
  // pantalla en blanco bajo las pestañas no explica nada.
  const bool sectionsView = level == SECTIONS || (level == ITEMS && itemCount() == 0);
  std::string title = sectionsView ? std::string(tr(STR_HUB_REMINDERS)) : sectionTitle(sectionIndex);
  if (level == EDIT) title = edit.title.empty() ? std::string(tr(STR_REM_EDIT)) : edit.title;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  if (level == EDIT) {
    renderEditor();
    const auto editLabels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, editLabels.btn1, editLabels.btn2, editLabels.btn3, editLabels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  // El margen de abajo lleva verticalSpacing además del alto de los hints, o la
  // última fila queda pegada a la barra de botones y parece tapada.
  const int hintY = listui::contentBottom(renderer) - listui::HINT_H;
  const int pagerY = hintY - PAGER_H;
  const int rowsBottom = pagerY - listui::GAP;

  const char* hint = tr(STR_AGENDA_OPEN_HINT);
  if (sectionsView) {
    renderSections(x, listui::contentTop(), w, rowsBottom);
  } else {
    const int top = drawTabs(x, listui::contentTop(), w);
    renderItems(x, top, w, rowsBottom, pagerY);
    hint = current().kind == REMINDERS ? tr(STR_REM_OPEN_HINT) : tr(STR_AGENDA_ITEM_HINT);
  }
  listui::hint(renderer, hintY, hint);

  if (menuStep != NONE && menu.processRender(renderer, mappedInput)) return;
  const char* okLabel = tr(STR_SELECT);
  if (!sectionsView) okLabel = current().kind == REMINDERS ? tr(STR_SELECT) : tr(STR_AGENDA_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), okLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de refrescos limpios la lleva el coordinador del panel
  // (PanelRefreshCoordinator): acá ya no hay contador propio que se reiniciaba
  // en cada cambio de pantalla.
  renderer.displayBuffer();
}
