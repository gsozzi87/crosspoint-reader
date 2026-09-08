#pragma once

#include <ArduinoJson.h>
#include <I18n.h>

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
//   POST /api/trip/packing {tripId, id, done}
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
  enum State { TRIPS, DAYS, ITEMS, PACKING, ATT_TEXT, ATT_IMAGE, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, TRIPS_FETCH, TRIP_FETCH, ATT_FETCH, PAGE_FETCH };

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
  int dayIndex = 0;   // fila de la lista de días: 0 = cosas para llevar
  int itemIndex = 0;
  int packIndex = 0;
  int itemsPerPage = 1;
  int partialCount = 0;  // parciales desde el último refresco limpio
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

  int dayRowCount() const { return 1 + static_cast<int>(days.size()); }
  int selectedDay() const { return dayIndex - 1; }  // < 0 = la fila de cosas para llevar
  const TripDay* currentDay() const;

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
