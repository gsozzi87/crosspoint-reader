#include "NewsPack.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ServerClient.h>

#include <algorithm>

#include "util/CardRead.h"

namespace {
constexpr const char* TAG = "NEWSPACK";
constexpr const char* DIR = "/.crosspoint/news";
constexpr const char* MANIFEST = "/.crosspoint/news/pack.json";

std::string itemPath(const std::string& id) { return std::string(DIR) + "/" + id + ".txt"; }

// REV-059: el manifiesto y el cuerpo de una nota se leen CON TOPE. Este camino
// lo recorre la pantalla de sueño al suspender y al apagar, y una caché
// corrupta no puede tener permiso para impedir que el aparato duerma.
std::string readAll(const std::string& path) { return cardread::readCapped(TAG, path, cardread::CAP_JSON_CACHE); }

bool writeAll(const std::string& path, const std::string& data) {
  HalFile f;
  if (!Storage.openFileForWrite(TAG, path, f)) return false;
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  f.close();
  return n == data.size();
}

// El archivo de una nota: sha en el primer renglón (para saber si cambió sin
// bajarla) y el cuerpo después del renglón en blanco.
std::string sha1line(const std::string& raw) {
  const size_t nl = raw.find('\n');
  return nl == std::string::npos ? "" : raw.substr(0, nl);
}
}  // namespace

std::vector<newspack::Item> newspack::cached() {
  std::vector<Item> out;
  const std::string raw = readAll(MANIFEST);
  if (raw.empty()) return out;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return out;
  for (JsonVariantConst iv : doc["items"].as<JsonArrayConst>()) {
    Item it;
    it.id = iv["id"] | "";
    if (it.id.empty()) continue;
    it.feed = iv["feed"] | "";
    it.title = iv["title"] | "";
    it.when = iv["when"] | "";
    it.sha = iv["sha"] | "";
    it.chewed = iv["chewed"] | false;
    it.local = Storage.exists(itemPath(it.id).c_str());
    out.push_back(std::move(it));
  }
  return out;
}

std::string newspack::body(const std::string& id) {
  const std::string raw = readAll(itemPath(id));
  if (raw.empty()) return "";
  const size_t sep = raw.find("\n\n");
  return sep == std::string::npos ? raw : raw.substr(sep + 2);
}

std::vector<std::string> newspack::headlines(const int max) {
  std::vector<std::string> out;
  for (const Item& it : cached()) {
    if (it.title.empty()) continue;
    out.push_back(it.title);
    if (static_cast<int>(out.size()) >= max) break;
  }
  return out;
}

std::string newspack::fetchOne(const std::string& id) {
  if (id.empty()) return "";
  Storage.ensureDirectoryExists(DIR);
  ServerClient::Response one;
  if (SERVER_CLIENT.get("/api/news/item?id=" + id, one) != ServerClient::Result::Ok) {
    LOG_ERR(TAG, "no se pudo traer la nota %s (%d)", id.c_str(), one.status);
    return "";
  }
  JsonDocument doc;
  if (deserializeJson(doc, one.body) != DeserializationError::Ok) {
    LOG_ERR(TAG, "la nota %s llegó ilegible", id.c_str());
    return "";
  }
  const std::string text = doc["text"] | "";
  if (text.empty()) return "";
  // Se guarda con el sha que dice el servidor, así la sincronización siguiente
  // la reconoce como al día y no la vuelve a pedir.
  const std::string sha = doc["sha"] | "";
  writeAll(itemPath(id), sha + "\n\n" + text);
  return text;
}

int newspack::sync(const int budget) {
  Storage.ensureDirectoryExists(DIR);

  ServerClient::Response resp;
  if (SERVER_CLIENT.get("/api/news/pack", resp) != ServerClient::Result::Ok) {
    LOG_ERR(TAG, "no se pudo traer el manifiesto (%d)", resp.status);
    return -1;
  }
  // El manifiesto se guarda ANTES de bajar los cuerpos: si la conexión se corta
  // a la mitad, la próxima pasada sabe exactamente qué le falta.
  if (!writeAll(MANIFEST, resp.body)) {
    LOG_ERR(TAG, "no se pudo guardar el manifiesto");
    return -1;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    LOG_ERR(TAG, "manifiesto ilegible");
    return -1;
  }

  std::vector<std::string> vigentes;
  int bajadas = 0;
  for (JsonVariantConst iv : doc["items"].as<JsonArrayConst>()) {
    const std::string id = iv["id"] | "";
    if (id.empty()) continue;
    vigentes.push_back(id);
    const std::string sha = iv["sha"] | "";
    const std::string path = itemPath(id);
    // REV-085: SIN sha, EL SERVIDOR TODAVÍA NO TIENE EL CUERPO.
    //
    // El repaso de cada hora ya no entra a los diarios ni gasta modelo: deja el
    // titular anunciado y el cuerpo para cuando alguien ABRA la nota. Pedirlo
    // acá haría justo lo que este cambio saca — una llamada al modelo por nota
    // anunciada, se lea o no —, así que la sincronización se lleva sólo lo que
    // ya está hecho. El titular igual se ve: sale del manifiesto.
    if (sha.empty()) continue;
    // Ya está y es la misma versión: no se vuelve a bajar. Esto es lo que hace
    // que la segunda sincronización del día no gaste nada.
    if (Storage.exists(path.c_str()) && sha1line(readAll(path)) == sha) continue;
    if (budget > 0 && bajadas >= budget) continue;

    ServerClient::Response one;
    if (SERVER_CLIENT.get("/api/news/item?id=" + id, one) != ServerClient::Result::Ok) {
      LOG_ERR(TAG, "no se pudo bajar %s (%d)", id.c_str(), one.status);
      continue;
    }
    JsonDocument bodyDoc;
    if (deserializeJson(bodyDoc, one.body) != DeserializationError::Ok) continue;
    const std::string text = bodyDoc["text"] | "";
    if (text.empty()) continue;
    if (writeAll(path, sha + "\n\n" + text)) ++bajadas;
  }

  // Lo que ya no está en el manifiesto se borra: si no, la tarjeta junta notas
  // de hace meses que nadie va a volver a abrir.
  int borradas = 0;
  for (const String& name : Storage.listFiles(DIR, 200)) {
    const std::string file = name.c_str();
    if (file == "pack.json" || file.size() < 5 || file.compare(file.size() - 4, 4, ".txt") != 0) continue;
    const std::string id = file.substr(0, file.size() - 4);
    if (std::find(vigentes.begin(), vigentes.end(), id) != vigentes.end()) continue;
    if (Storage.remove((std::string(DIR) + "/" + file).c_str())) ++borradas;
  }

  LOG_INF(TAG, "paquete: %d notas, %d bajadas, %d viejas borradas", (int)vigentes.size(), bajadas, borradas);
  return bajadas;
}
