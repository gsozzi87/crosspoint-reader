#pragma once

#include <ArduinoJson.h>
#include <I18n.h>

#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Viaje día por día: los días del viaje, lo que hay en cada uno con su hora y,
// sobre un ítem con adjunto, PRIMERO el texto que sacó el servidor del PDF
// (vuelo, hora, puerta, asiento, código de reserva: eso se lee siempre) y desde
// ahí la imagen de la página, que es donde está el código de embarque. También
// la lista de cosas para llevar, donde OK tilda.
//
// Servidor (contrato acordado con el Hono):
//   GET /api/trips?lang=xx           -> {ok, trips:[{id, name, place, start, end}]}
//   GET /api/trip?id=&lang=xx        -> {ok, trip:{id, name,
//                                        days:[{date, note, items:[{id, at, title, kind,
//                                               kindLabel, place, note,
//                                               attachments:[{id, pages, ...}]}]}],
//                                        packing:[{id, text, done}]}}
//   GET /api/attachment/info?id=     -> {ok, attachment:{extracted, fields:[{label, value}],
//                                        pages, codes:[{page, ...}], warn}}
//   GET /api/attachment?id=&page=    -> BMP listo para la pantalla, igual que
//                                       /api/photos/file (el aparato solo lo pinta)
//   POST /api/trip/packing {tripId, id, done}   (tildar)
//   POST /api/trip/packing {tripId, text}       (agregar lo que sugirió la IA)
//   GET  /api/suggest/trip?id=&lang=[&refresh=1]
//       -> {ok, at, lines:[...], packing:[...]}  sugerencias del viaje: qué visitar
//          y en qué horario, y qué FALTA llevar. Cuestan plata (el servidor busca
//          en internet), así que se muestran las cacheadas y solo OK las recalcula.
//          Lo que dice que falta NO se agrega solo: se ofrece y se agrega con OK.
// Todo se cachea en la SD (/.crosspoint/trips.json y /.crosspoint/att/), así que
// el viaje se mira sin WiFi, que es como se llega al aeropuerto.
class TripActivity final : public Activity {
 public:
  explicit TripActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Trip", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

 private:
  enum State { TRIPS, DAYS, ITEMS, PACKING, SUGGEST, ATT_TEXT, ATT_IMAGE, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, TRIPS_FETCH, TRIP_FETCH, ATT_FETCH, PAGE_FETCH, SUGGEST_FETCH };

  struct TripRef {
    std::string id;
    std::string title;
    std::string when;
  };
  struct TripItem {
    std::string at;
    std::string title;
    std::string kind;
    std::string place;
    std::string note;
    std::vector<std::string> attachmentIds;
  };
  struct TripDay {
    std::string date;   // "2026-09-14"
    std::string label;  // la nota del día que manda el servidor, puede venir vacía
    std::vector<TripItem> items;
  };
  struct PackItem {
    std::string id;
    std::string text;
    bool done = false;
  };

  State state = DAYS;
  Pending pending = NONE;
  std::vector<TripRef> trips;
  std::string tripId;
  std::string tripTitle;
  std::vector<TripDay> days;
  std::vector<PackItem> packing;

  int tripIndex = 0;
  int dayIndex = 0;   // fila de la lista del viaje (ver DAY_ROW_*)
  int itemIndex = 0;
  int packIndex = 0;
  int itemsPerPage = 1;
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;

  // Adjunto abierto
  std::string attId;
  std::string attText;
  int attPages = 0;
  int attPage = 0;  // página que se está mirando (0-based)
  std::vector<std::string> attLines;
  int attLine = 0;       // primera línea de la página de texto en pantalla
  int attLinesPerPage = 1;

  // Filas de la lista del viaje: 0 = sugerencias, 1 = cosas para llevar y
  // después un día cada una.
  static constexpr int DAY_ROW_SUGGEST = 0;
  static constexpr int DAY_ROW_PACKING = 1;
  static constexpr int DAY_ROW_FIRST = 2;
  int dayRowCount() const { return DAY_ROW_FIRST + static_cast<int>(days.size()); }
  int selectedDay() const { return dayIndex - DAY_ROW_FIRST; }  // < 0 = una de las dos filas fijas
  const TripDay* currentDay() const;

  // --- Sugerencias del viaje ------------------------------------------------
  std::vector<std::string> suggestLines;    // qué visitar, a qué hora, cuánto se tarda
  std::vector<std::string> suggestPacking;  // lo que el modelo dice que falta llevar
  std::string suggestTripId;                // de qué viaje son las que tenemos
  time_t suggestAt = 0;
  std::string suggestError;
  bool suggestRefresh = false;
  int suggestTop = 0;
  int suggestPerPage = 1;

  bool fetchSuggest(bool refresh);
  void addSuggestedPacking(int index);
  void renderSuggest();

  void parseTripList(JsonVariantConst arr);
  void parseTrip(JsonVariantConst trip);
  void parseAttachmentInfo(JsonVariantConst doc);
  bool loadCache();
  void saveCache() const;
  bool fetchTrips();
  bool fetchTrip(const std::string& id);
  bool fetchAttachmentInfo(const std::string& id);
  bool fetchAttachmentPage(const std::string& id, int page);
  static std::string safeName(const std::string& id);
  static std::string pagePath(const std::string& id, int page);

  void openTrip();
  void openItem();
  void openImage();
  void togglePacking();
  void layoutAttachmentText();
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");

  void renderList();
  void renderAttachmentText();
  void drawImagePage();
};
