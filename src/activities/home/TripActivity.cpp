#include "TripActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CalendarActivity.h"
#include "MappedInputManager.h"
#include "PhotosActivity.h"
#include "SilentRestart.h"
#include "activities/ListStyle.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/Selection.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"

namespace {
constexpr const char* TAG = "TRIP";
constexpr const char* CACHE = "/.crosspoint/trips.json";
constexpr const char* ATT_DIR = "/.crosspoint/att";
constexpr int SIDE = listui::SIDE;
constexpr int HINT_H = listui::HINT_H;
constexpr int PAGER_H = 24;
constexpr unsigned long REFRESH_HOLD_MS = 1200;

// El día del viaje se muestra como "Lunes 14 · Madrid": la fecha en palabras
// sale de las mismas tablas que el calendario.
std::string dayHeading(const std::string& date, const std::string& label) {
  std::string out;
  if (date.size() >= 10) {
    const int y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
    const int m = (date[5] - '0') * 10 + (date[6] - '0');
    const int d = (date[8] - '0') * 10 + (date[9] - '0');
    if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
      out = std::string(CalendarActivity::weekdayName(CalendarActivity::weekdayOfCivil(y, m, d))) + " " +
            std::to_string(d);
    }
  }
  if (!label.empty()) out += out.empty() ? label : " · " + label;
  return out;
}
}  // namespace

std::string TripActivity::safeName(const std::string& id) {
  std::string out;
  for (const char c : id) {
    out += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    if (out.size() >= 40) break;
  }
  return out.empty() ? std::string("x") : out;
}

std::string TripActivity::pagePath(const std::string& id, const int page) {
  return std::string(ATT_DIR) + "/" + safeName(id) + "-" + std::to_string(page) + ".bmp";
}

const TripActivity::TripDay* TripActivity::currentDay() const {
  const int i = selectedDay();
  if (i < 0 || i >= static_cast<int>(days.size())) return nullptr;
  return &days[i];
}

void TripActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.ensureDirectoryExists(ATT_DIR);
  if (loadCache() && !days.empty()) {
    state = DAYS;
    requestUpdate();
    return;
  }
  if (!trips.empty()) {
    state = TRIPS;
    requestUpdate();
    return;
  }
  pending = TRIPS_FETCH;
  ensureConnected();
}

void TripActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void TripActivity::fail(const StrId why, std::string detail) {
  LOG_ERR(TAG, "%s %s", I18N.get(why), detail.c_str());
  failureId = why;
  failureDetail = std::move(detail);
  state = FAILED;
  requestUpdate();
}

bool TripActivity::loadCache() {
  if (!Storage.exists(CACHE)) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, CACHE, f)) return false;
  std::string raw;
  raw.resize(f.size());
  const int got = raw.empty() ? 0 : f.read(&raw[0], raw.size());
  f.close();
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  parseTripList(doc["trips"]);
  parseTrip(doc["trip"]);
  JsonVariantConst sg = doc["suggest"];
  if (!sg.isNull()) {
    suggestTripId = sg["tripId"] | "";
    suggestAt = static_cast<time_t>(sg["at"] | (int64_t)0);
    for (JsonVariantConst lv : sg["lines"].as<JsonArrayConst>()) {
      const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
      if (!line.empty()) suggestLines.push_back(line);
    }
    for (JsonVariantConst lv : sg["packing"].as<JsonArrayConst>()) {
      const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
      if (!line.empty()) suggestPacking.push_back(line);
    }
    // Las del viaje anterior no valen para este.
    if (!tripId.empty() && suggestTripId != tripId) {
      suggestLines.clear();
      suggestPacking.clear();
      suggestAt = 0;
    }
  }
  return !days.empty() || !packing.empty() || !trips.empty();
}

// El viaje llega igual desde el servidor y desde la caché de la tarjeta. El
// título del viaje es `name` en el Hono (y `title` en la caché vieja), y los
// adjuntos de un ítem vienen como objetos `attachments[{id, pages, ...}]`; se
// acepta también la lista de ids suelta.
void TripActivity::parseTripList(JsonVariantConst arr) {
  trips.clear();
  for (JsonVariantConst tv : arr.as<JsonArrayConst>()) {
    TripRef ref;
    ref.id = tv["id"] | "";
    ref.title = tv["name"] | "";
    if (ref.title.empty()) ref.title = tv["title"] | "";
    ref.when = tv["start"] | "";
    const std::string place = tv["place"] | "";
    if (!place.empty()) ref.when += ref.when.empty() ? place : " \u00b7 " + place;
    if (!ref.id.empty()) trips.push_back(std::move(ref));
  }
}

void TripActivity::parseTrip(JsonVariantConst trip) {
  tripId = trip["id"] | "";
  tripTitle = trip["name"] | "";
  if (tripTitle.empty()) tripTitle = trip["title"] | "";
  days.clear();
  for (JsonVariantConst dv : trip["days"].as<JsonArrayConst>()) {
    TripDay day;
    day.date = dv["date"] | "";
    day.label = dv["label"] | "";
    if (day.label.empty()) day.label = dv["note"] | "";
    for (JsonVariantConst iv : dv["items"].as<JsonArrayConst>()) {
      TripItem it;
      it.at = iv["at"] | "";
      it.title = iv["title"] | "";
      it.kind = iv["kindLabel"] | "";
      if (it.kind.empty()) it.kind = iv["kind"] | "";
      it.place = iv["place"] | "";
      it.note = iv["note"] | "";
      for (JsonVariantConst av : iv["attachments"].as<JsonArrayConst>()) {
        const std::string aid = av["id"] | "";
        if (!aid.empty()) it.attachmentIds.push_back(aid);
      }
      for (JsonVariantConst av : iv["attachmentIds"].as<JsonArrayConst>()) {
        const std::string aid = av.as<const char*>() ? av.as<const char*>() : "";
        if (!aid.empty()) it.attachmentIds.push_back(aid);
      }
      day.items.push_back(std::move(it));
    }
    days.push_back(std::move(day));
  }
  packing.clear();
  for (JsonVariantConst pv : trip["packing"].as<JsonArrayConst>()) {
    packing.push_back({pv["id"] | "", pv["text"] | "", pv["done"] | false});
  }
}

void TripActivity::saveCache() const {
  JsonDocument doc;
  JsonArray ts = doc["trips"].to<JsonArray>();
  for (const TripRef& t : trips) {
    JsonObject o = ts.add<JsonObject>();
    o["id"] = t.id;
    o["name"] = t.title;
    o["start"] = t.when;
  }
  JsonObject trip = doc["trip"].to<JsonObject>();
  trip["id"] = tripId;
  trip["name"] = tripTitle;
  JsonArray ds = trip["days"].to<JsonArray>();
  for (const TripDay& d : days) {
    JsonObject o = ds.add<JsonObject>();
    o["date"] = d.date;
    o["label"] = d.label;
    JsonArray is = o["items"].to<JsonArray>();
    for (const TripItem& it : d.items) {
      JsonObject io = is.add<JsonObject>();
      io["at"] = it.at;
      io["title"] = it.title;
      io["kind"] = it.kind;
      io["place"] = it.place;
      io["note"] = it.note;
      JsonArray as = io["attachmentIds"].to<JsonArray>();
      for (const std::string& a : it.attachmentIds) as.add(a);
    }
  }
  JsonArray ps = trip["packing"].to<JsonArray>();
  for (const PackItem& p : packing) {
    JsonObject o = ps.add<JsonObject>();
    o["id"] = p.id;
    o["text"] = p.text;
    o["done"] = p.done;
  }
  // Las sugerencias van al lado del viaje: se ven sin WiFi y no se vuelven a
  // pedir (o sea, a pagar) por entrar y salir de la pantalla.
  JsonObject sg = doc["suggest"].to<JsonObject>();
  sg["tripId"] = suggestTripId;
  sg["at"] = static_cast<int64_t>(suggestAt);
  JsonArray sl = sg["lines"].to<JsonArray>();
  for (const std::string& line : suggestLines) sl.add(line);
  JsonArray sp = sg["packing"].to<JsonArray>();
  for (const std::string& line : suggestPacking) sp.add(line);
  std::string raw;
  serializeJson(doc, raw);
  HalFile f;
  if (!Storage.openFileForWrite(TAG, CACHE, f)) return;
  f.write(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
  f.close();
}

bool TripActivity::fetchTrips() {
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/trips?lang=" + std::string(uiLanguageCode()), resp);
  if (r != ServerClient::Result::Ok) {
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/trips: %s", failureDetail.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  parseTripList(doc["trips"]);
  saveCache();
  return true;
}

bool TripActivity::fetchTrip(const std::string& id) {
  ServerClient::Response resp;
  const std::string path = "/api/trip?id=" + id + "&lang=" + std::string(uiLanguageCode());
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok) {
    // 404 = el viaje se borró desde la web. Eso no es un error que haya que
    // mostrarle a nadie: es la caché del aparato que quedó vieja. Se tira lo
    // guardado y se vuelve a la lista, que es lo que el usuario espera ver.
    if (resp.status == 404) {
      LOG_INF(TAG, "el viaje %s ya no está en el servidor: se saca de la caché", id.c_str());
      days.clear();
      packing.clear();
      attText.clear();
      suggestLines.clear();
      suggestPacking.clear();
      suggestAt = 0;
      suggestTripId.clear();
      if (tripId == id) tripId.clear();
      trips.erase(std::remove_if(trips.begin(), trips.end(), [&id](const TripRef& t) { return t.id == id; }),
                  trips.end());
      saveCache();
      return fetchTrips();  // la lista fresca, sin el que ya no está
    }
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/trip: %s", failureDetail.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  // El viaje puede venir en `trip` o suelto en la raíz.
  JsonVariantConst trip = doc["trip"];
  if (trip.isNull() || trip["days"].isNull()) trip = doc.as<JsonVariantConst>();
  parseTrip(trip);
  if (tripId.empty()) tripId = id;
  // Las sugerencias son de un viaje: al cambiar de viaje no valen más.
  if (suggestTripId != tripId) {
    suggestLines.clear();
    suggestPacking.clear();
    suggestAt = 0;
    suggestTripId.clear();
  }
  saveCache();
  return !days.empty() || !packing.empty();
}

// Lo que el servidor sacó del PDF. Primero los datos sueltos (vuelo, hora,
// puerta, asiento, reserva), que es lo que se lee de un vistazo, después el
// aviso si el código puede no escanear y al final el texto largo. Se guarda la
// respuesta al lado del bitmap: es lo que se lee siempre, también sin WiFi.
void TripActivity::parseAttachmentInfo(JsonVariantConst doc) {
  JsonVariantConst att = doc["attachment"];
  if (att.isNull()) att = doc;
  attText.clear();
  for (JsonVariantConst fv : att["fields"].as<JsonArrayConst>()) {
    const std::string label = fv["label"] | "";
    const std::string value = fv["value"] | "";
    if (value.empty()) continue;
    attText += (label.empty() ? value : label + ": " + value) + "\n";
  }
  const std::string warn = att["warn"] | "";
  if (!warn.empty()) attText += warn + "\n";
  std::string body = att["extracted"] | "";
  if (body.empty()) body = att["text"] | "";
  if (!body.empty()) attText += (attText.empty() ? "" : "\n") + body;
  attPages = att["pages"] | 0;
  // La página que muestra el código de embarque: es la que hay que abrir.
  attPage = 0;
  for (JsonVariantConst cv : att["codes"].as<JsonArrayConst>()) {
    const int page = cv["page"] | -1;
    if (page >= 0 && page < attPages) {
      attPage = page;
      break;
    }
  }
}

bool TripActivity::fetchAttachmentInfo(const std::string& id) {
  const std::string path = std::string(ATT_DIR) + "/" + safeName(id) + ".json";
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get("/api/attachment/info?id=" + id, resp);
  if (r != ServerClient::Result::Ok) {
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/attachment/info: %s", failureDetail.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  parseAttachmentInfo(doc);
  HalFile f;
  if (Storage.openFileForWrite(TAG, path, f)) {
    f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
    f.close();
  }
  return true;
}

bool TripActivity::fetchAttachmentPage(const std::string& id, const int page) {
  ServerClient::Response resp;
  const std::string path = "/api/attachment?id=" + id + "&page=" + std::to_string(page);
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  if (r != ServerClient::Result::Ok || resp.body.size() < 100) {
    failureDetail = std::string(ServerClient::resultName(r)) + " " + std::to_string(resp.status);
    LOG_ERR(TAG, "GET /api/attachment: %s", failureDetail.c_str());
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite(TAG, pagePath(id, page), f)) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
  f.close();
  return written == resp.body.size();
}

void TripActivity::openTrip() {
  if (trips.empty()) return;
  const std::string id = trips[std::min(tripIndex, static_cast<int>(trips.size()) - 1)].id;
  tripId = id;
  pending = TRIP_FETCH;
  ensureConnected();
}

// OK sobre un ítem: si tiene adjunto se abre PRIMERO el texto (vuelo, hora,
// puerta, asiento, reserva), y desde ahí se pasa a la imagen de la página.
void TripActivity::openItem() {
  const TripDay* day = currentDay();
  if (!day || itemIndex >= static_cast<int>(day->items.size())) return;
  const TripItem& item = day->items[itemIndex];
  if (item.attachmentIds.empty()) return;
  attId = item.attachmentIds[0];
  attText.clear();
  attPages = 0;
  attPage = 0;
  attLine = 0;
  // Lo cacheado primero: en el aeropuerto puede no haber red.
  HalFile f;
  const std::string info = std::string(ATT_DIR) + "/" + safeName(attId) + ".json";
  if (Storage.openFileForRead(TAG, info, f)) {
    std::string raw;
    raw.resize(f.size());
    const int got = raw.empty() ? 0 : f.read(&raw[0], raw.size());
    f.close();
    JsonDocument doc;
    if (got > 0 && deserializeJson(doc, raw) == DeserializationError::Ok) {
      parseAttachmentInfo(doc);
      state = ATT_TEXT;
      attLines.clear();
      requestUpdate();
      return;
    }
  }
  pending = ATT_FETCH;
  ensureConnected();
}

void TripActivity::openImage() {
  if (attPages <= 0) return;
  if (Storage.exists(pagePath(attId, attPage).c_str())) {
    state = ATT_IMAGE;
    requestUpdate();
    return;
  }
  pending = PAGE_FETCH;
  ensureConnected();
}

void TripActivity::togglePacking() {
  if (packIndex < 0 || packIndex >= static_cast<int>(packing.size())) return;
  PackItem& p = packing[packIndex];
  p.done = !p.done;
  saveCache();
  // Lo que se acaba de agregar desde una sugerencia todavía no tiene id (lo pone
  // el servidor): el tilde queda local hasta la próxima vez que se baje el
  // viaje. Mandarlo sin id crearía un ítem repetido.
  if (p.id.empty()) {
    requestUpdate();
    return;
  }
  std::string body;
  {
    // `done` explícito (no un tilde que alterna): un reintento de la cola
    // offline tiene que dejar el ítem como quedó acá, no darlo vuelta otra vez.
    JsonDocument doc;
    doc["tripId"] = tripId;
    doc["id"] = p.id;
    doc["done"] = p.done;
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "packing %s: %s", p.id.c_str(),
          ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/trip/packing", body)));
  requestUpdate();
}

// GET /api/suggest/trip. `refresh` solo cuando lo pidió el usuario con OK: cada
// recálculo hace buscar al servidor y eso cuesta plata.
bool TripActivity::fetchSuggest(const bool refresh) {
  std::string path = "/api/suggest/trip?lang=" + std::string(uiLanguageCode());
  if (!tripId.empty()) path += "&id=" + tripId;
  if (refresh) path += "&refresh=1";
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
  suggestError.clear();
  if (r != ServerClient::Result::Ok) {
    // 429 = el servidor ya gastó las sugerencias del día; se dice tal cual.
    suggestError = resp.status == 429 ? tr(STR_SUGGEST_BUDGET) : tr(STR_SUGGEST_FAILED);
    LOG_ERR(TAG, "GET /api/suggest/trip: %s %d", ServerClient::resultName(r), resp.status);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    suggestError = tr(STR_SUGGEST_FAILED);
    return false;
  }
  suggestLines.clear();
  suggestPacking.clear();
  for (JsonVariantConst lv : doc["lines"].as<JsonArrayConst>()) {
    const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
    if (!line.empty()) suggestLines.push_back(line);
  }
  for (JsonVariantConst lv : doc["packing"].as<JsonArrayConst>()) {
    const std::string line = lv.as<const char*>() ? lv.as<const char*>() : "";
    // Lo que ya está anotado no se ofrece de nuevo.
    bool already = false;
    for (const PackItem& p : packing) already = already || p.text == line;
    if (!line.empty() && !already) suggestPacking.push_back(line);
  }
  suggestAt = static_cast<time_t>(doc["at"] | (int64_t)0);
  suggestTripId = tripId;
  if (suggestLines.empty() && suggestPacking.empty()) suggestError = tr(STR_SUGGEST_EMPTY);
  else saveCache();
  return !suggestLines.empty() || !suggestPacking.empty();
}

// OK sobre algo que el modelo dice que falta: recién ahí se agrega a la lista
// de cosas para llevar (nunca solo). El id lo pone el servidor; hasta que
// conteste, la fila se muestra con lo que se escribió.
void TripActivity::addSuggestedPacking(const int index) {
  if (index < 0 || index >= static_cast<int>(suggestPacking.size())) return;
  const std::string text = suggestPacking[index];
  suggestPacking.erase(suggestPacking.begin() + index);
  packing.push_back({"", text, false});
  saveCache();
  std::string body;
  {
    JsonDocument doc;
    doc["tripId"] = tripId;
    doc["text"] = text;
    serializeJson(doc, body);
  }
  LOG_INF(TAG, "packing + %s: %s", text.c_str(),
          ServerClient::resultName(SERVER_CLIENT.postOrQueue("/api/trip/packing", body)));
  requestUpdate();
}

void TripActivity::ensureConnected() {
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void TripActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    pending = NONE;
    wifiActivated = false;
    if (days.empty() && packing.empty()) {
      fail(StrId::STR_SERVER_WIFI_FAILED);
    } else {
      state = DAYS;
      requestUpdate();
    }
    return;
  }
  state = LOADING;
  requestUpdate();
}

void TripActivity::loop() {
  switch (state) {
    case LOADING: {
      WiFi.setSleep(false);
      const Pending p = pending;
      pending = NONE;
      if (p == SUGGEST_FETCH) {
        fetchSuggest(suggestRefresh);
        suggestRefresh = false;
        WiFi.setSleep(true);
        state = SUGGEST;
        suggestTop = 0;
        requestUpdate();
        break;
      }
      if (p == TRIPS_FETCH) {
        const bool ok = fetchTrips();
        WiFi.setSleep(true);
        if (!ok || trips.empty()) {
          fail(StrId::STR_ASK_FAILED, failureDetail);
          break;
        }
        if (trips.size() == 1) {  // un solo viaje: no hay nada que elegir
          tripId = trips[0].id;
          pending = TRIP_FETCH;
          state = LOADING;
          break;
        }
        state = TRIPS;
        tripIndex = 0;
        requestUpdate();
      } else if (p == TRIP_FETCH) {
        const bool ok = fetchTrip(tripId);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, failureDetail);
          break;
        }
        state = DAYS;
        dayIndex = 0;
        requestUpdate();
      } else if (p == ATT_FETCH) {
        const bool ok = fetchAttachmentInfo(attId);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, failureDetail);
          break;
        }
        attLines.clear();
        attLine = 0;
        state = ATT_TEXT;
        requestUpdate();
      } else if (p == PAGE_FETCH) {
        const bool ok = fetchAttachmentPage(attId, attPage);
        WiFi.setSleep(true);
        if (!ok) {
          fail(StrId::STR_ASK_FAILED, failureDetail);
          break;
        }
        state = ATT_IMAGE;
        requestUpdate();
      } else {
        state = DAYS;
        requestUpdate();
      }
      break;
    }
    case TRIPS: {
      const int count = static_cast<int>(trips.size());
      buttonNavigator.onNext([&] {
        if (count > 0) tripIndex = ButtonNavigator::nextIndex(tripIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) tripIndex = ButtonNavigator::previousIndex(tripIndex, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openTrip();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      break;
    }
    case DAYS: {
      if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, REFRESH_HOLD_MS)) {
        pending = tripId.empty() ? TRIPS_FETCH : TRIP_FETCH;
        ensureConnected();
        break;
      }
      const int count = dayRowCount();
      buttonNavigator.onNext([&] {
        dayIndex = ButtonNavigator::nextIndex(dayIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        dayIndex = ButtonNavigator::previousIndex(dayIndex, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (dayIndex == DAY_ROW_SUGGEST) {
          suggestTop = 0;
          state = SUGGEST;
          // Lo cacheado se muestra en el acto; pedirlas es cosa de OK ahí adentro.
          requestUpdate();
        } else if (dayIndex == DAY_ROW_PACKING) {
          state = PACKING;
          packIndex = 0;
          requestUpdate();
        } else {
          state = ITEMS;
          itemIndex = 0;
          requestUpdate();
        }
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        if (trips.size() > 1) {
          state = TRIPS;
          requestUpdate();
        } else {
          finish();
        }
      }
      break;
    }
    case ITEMS: {
      const TripDay* day = currentDay();
      const int count = day ? static_cast<int>(day->items.size()) : 0;
      buttonNavigator.onNext([&] {
        if (count > 0) itemIndex = ButtonNavigator::nextIndex(itemIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) itemIndex = ButtonNavigator::previousIndex(itemIndex, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openItem();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = DAYS;
        requestUpdate();
      }
      break;
    }
    case PACKING: {
      // Abajo de la lista van las que sugirió el modelo: OK sobre una la AGREGA.
      const int mine = static_cast<int>(packing.size());
      const int count = mine + static_cast<int>(suggestPacking.size());
      buttonNavigator.onNext([&] {
        if (count > 0) packIndex = ButtonNavigator::nextIndex(packIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        if (count > 0) packIndex = ButtonNavigator::previousIndex(packIndex, count);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (packIndex >= mine) addSuggestedPacking(packIndex - mine);
        else togglePacking();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = DAYS;
        requestUpdate();
      }
      break;
    }
    case SUGGEST: {
      const int total = static_cast<int>(suggestLines.size());
      buttonNavigator.onNext([&] {
        if (suggestTop + suggestPerPage < total) suggestTop += suggestPerPage;
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        suggestTop = std::max(0, suggestTop - suggestPerPage);
        requestUpdate();
      });
      // OK es lo único que le hace gastar una búsqueda al servidor.
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        suggestRefresh = !suggestLines.empty();
        pending = SUGGEST_FETCH;
        ensureConnected();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = DAYS;
        requestUpdate();
      }
      break;
    }
    case ATT_TEXT: {
      const int total = static_cast<int>(attLines.size());
      buttonNavigator.onNext([&] {
        if (attLine + attLinesPerPage < total) attLine += attLinesPerPage;
        requestUpdate();
      });
      buttonNavigator.onPrevious([&] {
        attLine = std::max(0, attLine - attLinesPerPage);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openImage();
        break;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = ITEMS;
        requestUpdate();
      }
      break;
    }
    case ATT_IMAGE: {
      // Cada repintado son tres pasadas de gris: solo se mueve de página cuando
      // el adjunto tiene más de una.
      if (attPages > 1) {
        buttonNavigator.onNextPress([&] {
          attPage = (attPage + 1) % attPages;
          openImage();
        });
        buttonNavigator.onPreviousPress([&] {
          attPage = (attPage - 1 + attPages) % attPages;
          openImage();
        });
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        state = ATT_TEXT;
        requestUpdate();
      }
      break;
    }
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (days.empty() && packing.empty()) {
          finish();
        } else {
          state = DAYS;
          requestUpdate();
        }
      }
      break;
    case CONNECTING:
      break;
  }
}

void TripActivity::layoutAttachmentText() {
  const int pageWidth = renderer.getScreenWidth();
  attLines = renderer.wrappedText(UI_10_FONT_ID, attText.c_str(), pageWidth - 2 * SIDE, 400);
}

void TripActivity::renderList() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int x = listui::SIDE;
  const int w = listui::contentWidth(renderer);
  const int top = listui::contentTop();
  // El margen de abajo lleva verticalSpacing además del alto de los hints, o la
  // última fila queda pegada a la barra de botones.
  const int hintY = listui::contentBottom(renderer) - HINT_H;
  const int pagerY = hintY - PAGER_H;
  itemsPerPage = std::max(1, (pagerY - listui::GAP - top) / listui::ROW2_H);

  int count = 0;
  int selected = 0;
  switch (state) {
    case TRIPS:
      count = static_cast<int>(trips.size());
      selected = tripIndex;
      break;
    case DAYS:
      count = dayRowCount();
      selected = dayIndex;
      break;
    case ITEMS:
      count = currentDay() ? static_cast<int>(currentDay()->items.size()) : 0;
      selected = itemIndex;
      break;
    case PACKING:
      count = static_cast<int>(packing.size() + suggestPacking.size());
      selected = packIndex;
      break;
    default:
      break;
  }

  if (count == 0) {
    const char* empty = state == ITEMS    ? tr(STR_TRIP_NO_ITEMS)
                        : state == PACKING ? tr(STR_AGENDA_EMPTY)
                                           : tr(STR_TRIP_NONE);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10,
                              renderer.truncatedText(UI_10_FONT_ID, empty, pageWidth - 2 * SIDE).c_str());
  }

  const int page = count > 0 ? selected / itemsPerPage : 0;
  const int first = page * itemsPerPage;
  for (int i = first; i < count && i < first + itemsPerPage; ++i) {
    const int y = top + (i - first) * listui::ROW2_H;
    std::string title;
    std::string detail;
    std::string meta;
    listui::RowSpec spec;
    spec.selected = i == selected;
    switch (state) {
      case TRIPS:
        title = trips[i].title;
        detail = trips[i].when;
        spec.bold = true;
        break;
      case DAYS:
        if (i == DAY_ROW_SUGGEST) {
          title = tr(STR_SUGGEST_TITLE);
          detail = tr(STR_TRIP_SUGGEST_SUB);
          if (!suggestPacking.empty()) meta = "+" + std::to_string(suggestPacking.size());
        } else if (i == DAY_ROW_PACKING) {
          title = tr(STR_TRIP_PACKING);
          int done = 0;
          for (const PackItem& p : packing) done += p.done ? 1 : 0;
          meta = std::to_string(done) + "/" + std::to_string(packing.size());
        } else {
          const TripDay& d = days[i - DAY_ROW_FIRST];
          title = dayHeading(d.date, d.label);
          if (title.empty()) title = d.date;
          meta = std::to_string(d.items.size());
        }
        spec.bold = true;
        break;
      case ITEMS: {
        const TripItem& it = currentDay()->items[i];
        title = it.title;
        detail = it.place;
        // La hora va a la derecha, en su columna: pegada al título se lee como
        // parte del nombre de la actividad.
        meta = it.at;
        if (!it.attachmentIds.empty()) detail += (detail.empty() ? "" : " · ") + std::string(tr(STR_TRIP_ATTACHMENT));
        break;
      }
      case PACKING:
        // Primero lo que ya está anotado, con su casilla (OK la tilda), y
        // después lo que sugirió el modelo, que TODAVÍA no está en la lista y
        // por eso no lleva casilla sino "Agregar".
        if (i < static_cast<int>(packing.size())) {
          title = packing[i].text;
          spec.check = true;
          spec.checked = packing[i].done;
        } else {
          title = suggestPacking[i - packing.size()];
          detail = tr(STR_TRIP_SUGGEST_ADD);
        }
        break;
      default:
        break;
    }
    spec.title = title.c_str();
    spec.detail = detail.empty() ? nullptr : detail.c_str();
    spec.meta = meta.empty() ? nullptr : meta.c_str();
    listui::row(renderer, x, y, w, listui::ROW2_H, spec);
  }
  listui::pager(renderer, x, pagerY, w, page + 1, count > 0 ? (count + itemsPerPage - 1) / itemsPerPage : 1);

  const char* hint = nullptr;
  if (state == ITEMS) hint = tr(STR_TRIP_ATTACH_HINT);
  else if (state == PACKING && !suggestPacking.empty()) hint = tr(STR_TRIP_SUGGEST_HINT);
  else if (state == PACKING) hint = tr(STR_TRIP_PACK_HINT);
  else if (state == DAYS) hint = tr(STR_CAL_REFRESH_HINT);
  listui::hint(renderer, hintY, hint);
}

// Las sugerencias del viaje, paginadas. Abajo dice cuándo se calcularon: son de
// la última vez que se pidieron, no de ahora.
void TripActivity::renderSuggest() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + 12;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - HINT_H;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  suggestPerPage = std::max(1, (bottom - top) / lineH);

  std::vector<std::string> lines;
  for (const std::string& line : suggestLines) {
    for (const std::string& part : renderer.wrappedText(UI_10_FONT_ID, ("• " + line).c_str(),
                                                        pageWidth - 2 * SIDE, 6)) {
      lines.push_back(part);
    }
  }
  if (lines.empty()) {
    const char* empty = suggestError.empty() ? tr(STR_SUGGEST_EMPTY) : suggestError.c_str();
    for (const std::string& part : renderer.wrappedText(UI_10_FONT_ID, empty, pageWidth - 2 * SIDE, 4)) {
      lines.push_back(part);
    }
  }
  if (suggestTop >= static_cast<int>(lines.size())) suggestTop = 0;
  for (int i = 0; i < suggestPerPage && suggestTop + i < static_cast<int>(lines.size()); ++i) {
    renderer.drawText(UI_10_FONT_ID, SIDE, top + i * lineH, lines[suggestTop + i].c_str());
  }

  std::string foot;
  time_t now = 0;
  if (suggestAt > 0 && halClock.getEpochUtc(now) && now > suggestAt) {
    const long mins = static_cast<long>(now - suggestAt) / 60;
    char buf[64];
    if (mins < 60) snprintf(buf, sizeof(buf), tr(STR_SUGGEST_AGE_MIN), static_cast<int>(mins));
    else snprintf(buf, sizeof(buf), tr(STR_SUGGEST_AGE_HOUR), static_cast<int>(mins / 60));
    foot = buf;
  }
  if (!suggestPacking.empty()) {
    char buf[64];
    snprintf(buf, sizeof(buf), tr(STR_TRIP_SUGGEST_MISSING), static_cast<int>(suggestPacking.size()));
    foot += foot.empty() ? buf : std::string("  ·  ") + buf;
  }
  if (!foot.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, bottom + 2,
                              renderer.truncatedText(SMALL_FONT_ID, foot.c_str(), pageWidth - 2 * SIDE).c_str());
  }
}

void TripActivity::renderAttachmentText() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  if (attLines.empty() && !attText.empty()) layoutAttachmentText();
  const int top = metrics.topPadding + metrics.headerHeight + 12;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - HINT_H;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  attLinesPerPage = std::max(1, (bottom - top) / lineH);
  if (attLines.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_TRIP_NO_TEXT));
  }
  for (int i = 0; i < attLinesPerPage && attLine + i < static_cast<int>(attLines.size()); ++i) {
    renderer.drawText(UI_10_FONT_ID, SIDE, top + i * lineH, attLines[attLine + i].c_str());
  }
  if (attPages > 0) {
    renderer.drawCenteredText(SMALL_FONT_ID, bottom + 2,
                              renderer.truncatedText(SMALL_FONT_ID, tr(STR_TRIP_IMAGE_HINT), pageWidth - 2 * SIDE).c_str());
  }
}

// La imagen de la página con el pipeline de grises del SDK: el mismo que usa
// PhotosActivity, no hay un segundo dibujante de BMP en el aparato.
void TripActivity::drawImagePage() {
  const int pageWidth = renderer.getScreenWidth();
  char page[40];
  snprintf(page, sizeof(page), tr(STR_TRIP_PAGE_FORMAT), attPage + 1, std::max(attPages, 1));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const auto overlay = [&] {
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (attPages > 1) {
      renderer.drawText(SMALL_FONT_ID, pageWidth / 2 - renderer.getTextWidth(SMALL_FONT_ID, page) / 2, 4, page);
    }
  };
  GUI.drawPopup(renderer, tr(STR_LOADING));
  if (PhotosActivity::drawFullScreenPhoto(renderer, pagePath(attId, attPage), overlay)) return;
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_FILE_OPEN_FAILED));
  overlay();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void TripActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (state == ATT_IMAGE) {
    drawImagePage();  // se encarga de su propio refresco (pipeline de grises)
    return;
  }

  renderer.clearScreen();
  std::string title = tripTitle.empty() ? std::string(tr(STR_TRIP_TITLE)) : tripTitle;
  if (state == ITEMS && currentDay()) {
    title = dayHeading(currentDay()->date, currentDay()->label);
    if (title.empty()) title = currentDay()->date;
  } else if (state == PACKING) {
    title = tr(STR_TRIP_PACKING);
  } else if (state == ATT_TEXT) {
    title = tr(STR_TRIP_ATTACHMENT);
  } else if (state == SUGGEST) {
    title = tr(STR_SUGGEST_TITLE);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  switch (state) {
    case TRIPS:
    case DAYS:
    case ITEMS:
    case PACKING:
      renderList();
      break;
    case SUGGEST:
      renderSuggest();
      break;
    case ATT_TEXT:
      renderAttachmentText();
      break;
    case LOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 10, tr(STR_TRIP_LOADING), true, EpdFontFamily::BOLD);
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, I18N.get(failureId), true, EpdFontFamily::BOLD);
      if (!failureDetail.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10,
                                  renderer.truncatedText(UI_10_FONT_ID, failureDetail.c_str(), pageWidth - 40).c_str());
      }
      break;
    default:
      break;
  }

  const char* okLabel = tr(STR_SELECT);
  if (state == PACKING) {
    // Sobre una fila sugerida, OK no tilda: agrega.
    okLabel = packIndex >= static_cast<int>(packing.size()) ? tr(STR_TRIP_SUGGEST_ADD) : tr(STR_AGENDA_DONE);
  } else if (state == ATT_TEXT) {
    okLabel = tr(STR_TRIP_ATTACHMENT);
  } else if (state == SUGGEST) {
    okLabel = suggestLines.empty() ? tr(STR_SUGGEST_ASK) : tr(STR_SUGGEST_REDO);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), okLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // La cadencia de refrescos limpios la lleva el coordinador del panel.
  renderer.displayBuffer();
}
