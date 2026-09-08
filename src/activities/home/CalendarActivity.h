#pragma once

#include <I18n.h>

#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Calendario: vista de mes (la cuadrícula de siempre, con un punto y la
// cantidad en cada día que tiene algo) y vista de día (lo que hay ese día con
// su hora). Se entra desde Recordatorios.
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
// Todo se cachea en /.crosspoint/calendar.json, como el clima y las noticias.
class CalendarActivity final : public Activity {
 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

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

 private:
  enum State { MONTH, DAY, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, MONTH_FETCH, DAY_FETCH };

  struct DaySummary {
    int day = 0;  // 1..31
    int count = 0;
    std::string firstTitle;
  };
  struct Item {
    std::string at;  // "10:30", vacío = todo el día
    std::string title;
    std::string place;
  };

  State state = MONTH;
  Pending pending = NONE;
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

  const DaySummary* summaryFor(int day) const;
  void moveDay(int delta);
  void openDay();
  void goToMonth(int year, int month, int day);

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
