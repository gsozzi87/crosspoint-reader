#include "SpeechToText.h"

#include <ArduinoJson.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerClient.h>
#include <WiFi.h>

#include "Lang.h"
#include "VoiceRecorder.h"

namespace {
constexpr const char* TAG = "STT";
constexpr uint32_t TRANSCRIBE_TIMEOUT_MS = 60000;  // Whisper on a 10 s clip
}  // namespace

bool SpeechToText::transcribe(const VoiceRecorder& take, std::string& text, std::string& detail) {
  text.clear();
  detail.clear();
  WiFi.setSleep(false);
  const uint8_t* body = const_cast<VoiceRecorder&>(take).adpcm();
  const size_t bytes = take.adpcmBytes();
  // adpcm() devuelve nullptr si no pudo reservar la PSRAM: sin esto se posteaba
  // un puntero nulo con el tamaño calculado.
  if (!body || bytes == 0) {
    WiFi.setSleep(true);
    detail = tr(STR_AUDIO_NO_MEMORY);
    return false;
  }
  LOG_DBG(TAG, "POST /api/transcribe: %u bytes", (unsigned)bytes);
  ServerClient::Response resp;
  const ServerClient::Result r = SERVER_CLIENT.postBytes(std::string("/api/transcribe?lang=") + uiLanguageCode(),
                                                        "audio/adpcm", body, bytes, resp, TRANSCRIBE_TIMEOUT_MS);
  if (r != ServerClient::Result::Ok) {
    WiFi.setSleep(true);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%d)", ServerClient::resultName(r), resp.status);
    detail = buf;
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    WiFi.setSleep(true);
    detail = tr(STR_VOICE_BAD_REPLY);
    return false;
  }
  text = doc["text"] | "";
  if (text.empty()) {
    WiFi.setSleep(true);
    detail = doc["error"] | tr(STR_VOICE_EMPTY_REPLY);
    return false;
  }
  LOG_DBG(TAG, "\"%s\"", text.c_str());
  return true;
}
