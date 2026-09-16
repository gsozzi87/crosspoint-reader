#include "LuaServiceActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
#include "Memory.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "voice/Lang.h"
#include "voice/SpeechToText.h"

namespace {
constexpr const char* TAG = "LUA_SERVICE";
constexpr const char* TRAVEL_CACHE = "/.crosspoint/travel-lua.json";
constexpr size_t MAX_EPUB = 2 * 1024 * 1024;

bool atomicWrite(const char* path, const uint8_t* data, const size_t size) {
  const std::string tmp = std::string(path) + ".part";
  const std::string bak = std::string(path) + ".bak";
  Storage.remove(tmp.c_str());
  HalFile file = Storage.open(tmp.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file || file.write(data, size) != size) {
    file.close();
    Storage.remove(tmp.c_str());
    return false;
  }
  file.flush();
  file.close();
  Storage.remove(bak.c_str());
  const bool had = Storage.exists(path);
  if (had && !Storage.rename(path, bak.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path)) {
    if (had) Storage.rename(bak.c_str(), path);
    Storage.remove(tmp.c_str());
    return false;
  }
  Storage.remove(bak.c_str());
  return true;
}

std::string safeBookName(const std::string& topic) {
  std::string out;
  bool dash = false;
  for (const unsigned char c : topic) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      dash = false;
    } else if (!out.empty() && !dash) {
      out.push_back('-');
      dash = true;
    }
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "investigacion" : out;
}
}  // namespace

void LuaServiceActivity::onEnter() {
  Activity::onEnter();
  if (action == "research_epub") {
    StrId why = StrId::STR_AUDIO_CAPTURE_FAILED;
    if (!recorder.start(why)) {
      fail(I18N.get(why));
      return;
    }
    state = RECORDING;
  } else {
    beginConnect();
  }
  requestUpdate();
}

void LuaServiceActivity::onExit() {
  recorder.abort();
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
  Activity::onExit();
}

void LuaServiceActivity::beginConnect() {
  wifiPicker = false;
  wifi.begin();
  state = CONNECTING;
  if (wifi.isDone()) pumpConnect();
  requestUpdate();
}

void LuaServiceActivity::pumpConnect() {
  if (wifiPicker) return;
  const uint32_t rev = wifi.revision();
  const FriendlyWifi::Phase phase = wifi.pump();
  if (phase == FriendlyWifi::Phase::Connected) {
    onConnected(true);
  } else if (phase == FriendlyWifi::Phase::NeedsPicker) {
    wifiPicker = true;
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false),
                           [this](const ActivityResult& result) {
                             wifiPicker = false;
                             onConnected(!result.isCancelled);
                           });
  } else if (wifi.revision() != rev) {
    requestUpdate();
  }
}

void LuaServiceActivity::onConnected(const bool ok) {
  if (!ok) {
    fail(tr(STR_SERVER_WIFI_FAILED));
    return;
  }
  state = WORKING;
  pending = true;
  requestUpdate();
}

void LuaServiceActivity::fail(std::string why) {
  detail = std::move(why);
  LOG_ERR(TAG, "%s", detail.c_str());
  state = FAILED;
  requestUpdate();
}

bool LuaServiceActivity::syncTravel() {
  JsonDocument cache;
  if (Storage.exists(TRAVEL_CACHE)) {
    const String old = Storage.readFile(TRAVEL_CACHE);
    deserializeJson(cache, old);
  }
  ServerClient::Response resp;
  const auto getJson = [&](const std::string& path, JsonDocument& out) {
    const ServerClient::Result r = SERVER_CLIENT.get(path, resp);
    if (r != ServerClient::Result::Ok) {
      detail = std::string(ServerClient::resultName(r)) + " (" + std::to_string(resp.status) + ")";
      return false;
    }
    return deserializeJson(out, resp.body) == DeserializationError::Ok;
  };

  const size_t colon = action.find(':');
  const std::string id = colon == std::string::npos ? "" : action.substr(colon + 1);
  if (action == "travel_refresh") {
    JsonDocument fresh;
    if (!getJson("/api/trips?lang=" + std::string(uiLanguageCode()), fresh)) return false;
    cache["trips"] = fresh["trips"];
  } else if (action.compare(0, 7, "travel:") == 0) {
    JsonDocument fresh;
    if (!getJson("/api/trip?id=" + id + "&lang=" + std::string(uiLanguageCode()), fresh)) return false;
    cache["trip"] = fresh["trip"];
  } else if (action.compare(0, 13, "travel_guide:") == 0) {
    JsonDocument fresh;
    if (!getJson("/api/suggest/trip?id=" + id + "&lang=" + std::string(uiLanguageCode()), fresh)) return false;
    JsonObject suggest = cache["suggest"].to<JsonObject>();
    suggest["tripId"] = id;
    suggest["at"] = fresh["at"];
    suggest["lines"] = fresh["lines"];
    suggest["packing"] = fresh["packing"];
  } else {
    detail = "acción de viaje desconocida";
    return false;
  }
  std::string raw;
  serializeJson(cache, raw);
  Storage.ensureDirectoryExists("/.crosspoint");
  return atomicWrite(TRAVEL_CACHE, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

bool LuaServiceActivity::makeResearchBook() {
  std::string transcribeDetail;
  if (!SpeechToText::transcribe(recorder, topic, transcribeDetail)) {
    detail = transcribeDetail;
    recorder.release();
    return false;
  }
  recorder.release();
  JsonDocument request;
  request["topic"] = topic;
  request["lang"] = uiLanguageCode();
  std::string body;
  serializeJson(request, body);
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postJson("/api/research/epub", body, resp, 150000);
  if (r != ServerClient::Result::Ok) {
    detail = std::string(ServerClient::resultName(r)) + " (" + std::to_string(resp.status) + ")";
    return false;
  }
  if (resp.body.size() < 4 || resp.body.size() > MAX_EPUB || resp.body[0] != 'P' || resp.body[1] != 'K') {
    detail = "el servidor no devolvió un EPUB válido";
    return false;
  }
  Storage.ensureDirectoryExists("/Books");
  Storage.ensureDirectoryExists("/Books/Investigaciones");
  bookPath = "/Books/Investigaciones/" + safeBookName(topic) + ".epub";
  return atomicWrite(bookPath.c_str(), reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
}

void LuaServiceActivity::runAction() {
  pending = false;
  WiFi.setSleep(false);
  const bool ok = action == "research_epub" ? makeResearchBook() : syncTravel();
  WiFi.setSleep(true);
  if (!ok) {
    fail(detail.empty() ? tr(STR_ASK_FAILED) : detail);
    return;
  }
  state = DONE;
  requestUpdate();
}

void LuaServiceActivity::loop() {
  switch (state) {
    case RECORDING:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        recorder.abort();
        finish();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !recorder.isRecording()) {
        recorder.stop();
        if (recorder.tooShort()) {
          fail(tr(STR_VOICE_NO_SPEECH));
        } else {
          beginConnect();
        }
      } else if (!recorder.pump()) {
        fail(tr(STR_AUDIO_CAPTURE_FAILED));
      }
      break;
    case CONNECTING:
      if (!wifiPicker && mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
      else pumpConnect();
      break;
    case WORKING:
      if (pending) runAction();
      break;
    case DONE:
      if (action == "research_epub" && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        onSelectBook(bookPath);
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                 mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        finish();
      }
      break;
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) finish();
      break;
  }
}

void LuaServiceActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 action == "research_epub" ? tr(STR_RESEARCH_TITLE) : tr(STR_TRIP_TITLE));
  const char* ok = "";
  if (state == RECORDING) {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 10, tr(STR_RESEARCH_PROMPT), true, EpdFontFamily::BOLD);
    ok = tr(STR_SELECT);
  } else if (state == CONNECTING) {
    if (!wifiPicker) FriendlyWifi::drawStatus(renderer, wifi, mid);
  } else if (state == WORKING) {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 10,
                              action == "research_epub" ? tr(STR_RESEARCH_WORKING) : tr(STR_TRIP_LOADING), true,
                              EpdFontFamily::BOLD);
  } else if (state == DONE) {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 20,
                              action == "research_epub" ? tr(STR_RESEARCH_SAVED) : tr(STR_HUB_SYNC_DONE), true,
                              EpdFontFamily::BOLD);
    if (action == "research_epub") {
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 20,
                                renderer.truncatedText(UI_10_FONT_ID, topic.c_str(), width - 48).c_str());
      ok = tr(STR_OPEN);
    }
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, mid - 10,
                              renderer.truncatedText(UI_10_FONT_ID, detail.c_str(), width - 48).c_str(), true,
                              EpdFontFamily::BOLD);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), ok, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
