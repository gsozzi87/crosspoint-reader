#pragma once

#include <I18n.h>

#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "voice/VoiceRecorder.h"

// "Mi día": el mosaico del hub que junta TODO lo que tiene fecha. Adentro hay
// tres pantallas, elegidas en un menú de tres filas:
//   Hoy         -> lo del día (agenda y viaje) más las sugerencias de la IA
//   Calendario  -> la cuadrícula del mes y la vista de un día
//   Viajes      -> TripActivity
// Antes el calendario y los viajes estaban escondidos como secciones adentro de
// Recordatorios; ahora son pestaña propia, que es como los pidió el usuario.
//
// Calendario: vista de mes (la cuadrícula de siempre, con un punto y la
// cantidad en cada día que tiene algo) y vista de día (lo que hay ese día con
// su hora).
//
// Con dos direcciones no hay forma de moverse en cruz, así que la palanca
// avanza y retrocede DE A UN DÍA: el cursor cruza de fila solo al llegar al
// domingo y cambia de mes al pasarse del último día. Es el mismo orden en el
// que se lee la cuadrícula, y la línea de abajo dice siempre qué día está
// marcado, así que nunca hay que adivinar dónde quedó el cursor.
//
// Servidor (contrato acordado con el Hono):
//   GET /api/calendar?from=YYYY-MM-DD&to=YYYY-MM-DD&lang=xx
//       -> {ok, days:[{date, count, firstTitle}], events:[{date, at, title, place}]}
//          `days` es el resumen que pinta el mes sin bajar todo; `events`, si
//          viene, deja el mes entero disponible sin WiFi.
//   GET /api/calendar/day?date=YYYY-MM-DD&lang=xx
//       -> {ok, items:[{at, title, place, note}]}
//   POST /api/calendar/dictate   {text, date, lang}
//       -> {ok, added:[{id, start, title, allDay}], reply}
//          El usuario DICTA el día entero ("a las 8 gimnasio, a las 9 reunión
//          con Ana") y el servidor lo parte en actividades. Necesita respuesta,
//          así que va con WiFi arriba, nunca por la cola.
//   POST /api/calendar/event         {id, ...campos...}  edita en su lugar
//   POST /api/calendar/event/delete  {id}
//          Los dos salen por `postOrQueue`: el cambio se ve en el acto y el
//          POST espera en la cola si no hay WiFi.
//   GET /api/suggest/day?date=YYYY-MM-DD&lang=xx[&refresh=1]
//       -> {ok, at, ageS, stale, lines:[...]}   sugerencias del día (server/src/suggest.ts)
//          Cuestan plata (el servidor busca en internet), así que NUNCA se piden
//          solas: se muestran las cacheadas y solo OK las recalcula (`refresh=1`).
// Todo se cachea en /.crosspoint/calendar.json, como el clima y las noticias.
class CalendarActivity final : public Activity {
 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == DICTATING; }
  bool preventAutoSleep() override {
    return state == CONNECTING || state == LOADING || state == DICTATING;
  }

  // Fechas, sin depender de la libc: las usa también la edición de
  // recordatorios de AgendaActivity.
  static const char* monthName(int month);    // 1..12
  static const char* weekdayName(int dow);    // 0 = lunes .. 6 = domingo
  static const char* weekdayShort(int dow);
  static int daysInMonth(int year, int month);
  static long daysFromCivil(int year, int month, int day);
  static void civilFromDays(long days, int& year, int& month, int& day);
  static int weekdayOfCivil(int year, int month, int day);  // 0 = lunes
  // Epoch UTC de una fecha y hora LOCALES (usa el huso de los ajustes).
  static time_t epochFromLocal(int year, int month, int day, int hour, int minute);
  // Al revés: de un epoch UTC a la fecha y hora locales.
  static void localFromEpoch(time_t epoch, int& year, int& month, int& day, int& hour, int& minute);
  // Hoy, según el RTC. Falso si el aparato no está en hora.
  static bool localToday(int& year, int& month, int& day);
  // "2026-09-14"
  static std::string isoDate(int year, int month, int day);

  // Una actividad del día. Además de lo que se pinta, lleva lo que hace falta
  // para volver a mandarla entera en POST /api/calendar/event: ese endpoint
  // REEMPLAZA el evento, así que lo que no se le manda (lugar, nota, fin,
  // repetición) se borraría del servidor.
  struct Item {
    int id = 0;
    std::string kind;  // "event" | "reminder" | "trip"; solo "event" se edita acá
    std::string at;    // "10:30", vacío = todo el día
    std::string title;
    std::string place;
    std::string note;
    std::string endTime;   // "HH:MM" de fin, vacío si no tiene
    std::string startAt;   // arranque de la SERIE ("2026-09-15T10:30"): con eso se edita
    std::string endStamp;  // fin de la serie, mismo formato
    std::string repeatRaw;  // el objeto `repeat` crudo, tal como vino
  };

 private:
  enum State { HOME, TODAY, MONTH, DAY, DICTATING, TIME_EDIT, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, MONTH_FETCH, DAY_FETCH, SUGGEST_FETCH, DICTATE_SEND, TITLE_SEND };
  // Qué se está grabando: el día entero o el título de una actividad.
  enum RecordMode { REC_DAY, REC_TITLE };
  // Filas del menú de entrada.
  enum HomeRow { ROW_TODAY, ROW_CALENDAR, ROW_TRIPS, HOME_ROWS };
  // Un renglón de la pantalla Hoy: el texto y con qué fuente se dibuja.
  struct Line {
    std::string text;
    uint8_t style;  // 0 = normal, 1 = título, 2 = chico
  };

  struct DaySummary {
    int day = 0;  // 1..31
    int count = 0;
    std::string firstTitle;
  };

  State state = HOME;
  State afterLoad = MONTH;  // adónde volver cuando termina de bajar algo
  Pending pending = NONE;
  int homeRow = ROW_TODAY;
  int viewYear = 0;  // mes en pantalla (0 = todavía no se sabe: sin reloj ni caché)
  int viewMonth = 0;
  int cursorDay = 1;
  std::vector<DaySummary> summary;  // del mes en pantalla
  std::vector<Item> dayItems;
  std::string dayDate;  // "YYYY-MM-DD" de lo que hay en dayItems
  int dayIndex = 0;
  int itemsPerPage = 1;
  int partialCount = 0;  // parciales desde el último refresco limpio
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  bool monthCached = false;  // el mes en pantalla salió de la caché o del servidor
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;

  // --- Hoy y las sugerencias ------------------------------------------------
  std::vector<std::string> suggestLines;
  std::string suggestDate;      // de qué día son las sugerencias que tenemos
  time_t suggestAt = 0;         // cuándo las calculó el servidor (epoch UTC)
  bool suggestRefresh = false;  // el próximo pedido es un recálculo pedido con OK
  std::string suggestError;
  std::vector<Line> todayLines;
  int todayTop = 0;             // primer renglón en pantalla
  int todayPerPage = 1;

  void openHomeRow();
  void goToCurrentMonth();
  void openToday();
  void buildTodayLines();
  bool fetchSuggest(bool refresh);
  bool loadSuggestFromCache(const std::string& date);
  void saveSuggestToCache() const;
  void renderHome();
  void renderToday();

  const DaySummary* summaryFor(int day) const;
  void moveDay(int delta);
  void openDay();
  void goToMonth(int year, int month, int day);

  // --- Dictar, editar y borrar las actividades del día ---------------------
  VoiceRecorder recorder{45};  // 45 s: lo que lleva enumerar una jornada entera
  RecordMode recordMode = REC_DAY;
  State dictateReturn = DAY;  // a qué pantalla se vuelve cuando termina de grabar
  Pending afterWifi = NONE;  // qué se estaba por hacer cuando se pidió el WiFi
  std::string dayNotice;     // lo que contestó el servidor, o por qué no se pudo
  std::string transcribed;   // lo que se entendió de la última grabación
  OptionPopup menu;
  bool menuOpen = false;
  std::vector<std::string> menuOptions;
  int menuItem = -1;  // índice en dayItems de la actividad del menú
  int editHour = -1;  // < 0 = sin hora (queda como algo del día)
  int editMinute = 0;
  bool editMinuteField = false;  // la palanca está sobre los minutos

  void startDictation(RecordMode mode);
  void stopDictation();
  void performDictate();
  void performTitle();
  void openItemMenu();
  void onMenuPick(int index);
  void openTimeEditor();
  void timeStep(int delta);
  void confirmTimeEditor();
  void deleteMenuItem();
  void sendEventEdit(const Item& it) const;
  void saveDayToCache() const;
  void syncSummaryCount();
  void sortDayItems();
  void renderDictating();
  void renderTimeEditor();

  bool loadMonthFromCache();
  bool loadDayFromCache(const std::string& date);
  bool fetchMonth();
  bool fetchDay(const std::string& date);
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");

  void renderMonth();
  void renderDay();
};
