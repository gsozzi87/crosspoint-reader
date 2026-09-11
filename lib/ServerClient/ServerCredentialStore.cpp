#include "ServerCredentialStore.h"

#include <Arduino.h>
#include <ObfuscationUtils.h>
#include <esp_random.h>

namespace {
constexpr uint8_t CONFIG_VERSION = 1;
constexpr size_t MAX_TOKEN_LENGTH = 256;

#ifndef CROSSPOINT_OTA_RELEASE_URL
#define CROSSPOINT_OTA_RELEASE_URL ""
#endif

// "https://host[:port]/anything" -> "https://host[:port]"
std::string originOf(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  const size_t slash = url.find('/', scheme + 3);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string trimSlash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}
}  // namespace

void ServerCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["serverUrl"] = serverUrl;
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
}

bool ServerCredentialStore::fromJson(JsonVariantConst doc) {
  setServerUrl(doc["serverUrl"] | "");
  const char* obf = doc["token_obf"] | "";
  bool ok = true;
  bool tooLong = false;
  std::string decoded = obfuscation::deobfuscateFromBase64(obf, MAX_TOKEN_LENGTH, &ok, &tooLong);
  token = (ok && !tooLong) ? decoded : "";
  return true;
}

void ServerCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  // Trim whitespace a web form may leave around the value.
  while (!serverUrl.empty() && (serverUrl.back() == ' ' || serverUrl.back() == '\r' || serverUrl.back() == '\n')) {
    serverUrl.pop_back();
  }
  while (!serverUrl.empty() && serverUrl.front() == ' ') serverUrl.erase(0, 1);
}

std::string ServerCredentialStore::getBaseUrl() const {
  if (!serverUrl.empty()) {
    if (serverUrl.find("://") == std::string::npos) return trimSlash("https://" + serverUrl);
    return trimSlash(serverUrl);
  }
  return originOf(CROSSPOINT_OTA_RELEASE_URL);
}

void ServerCredentialStore::setToken(const std::string& t) {
  token = t;
  while (!token.empty() && (token.back() == ' ' || token.back() == '\r' || token.back() == '\n')) token.pop_back();
  if (token.size() > MAX_TOKEN_LENGTH) token.resize(MAX_TOKEN_LENGTH);
}

std::string ServerCredentialStore::deviceId() {
  uint64_t mac = ESP.getEfuseMac();  // la MAC de fábrica, invariable
  char out[13];
  for (int i = 0; i < 6; ++i) {
    static const char kHexUpper[] = "0123456789ABCDEF";
    const uint8_t b = static_cast<uint8_t>((mac >> (8 * i)) & 0xFF);
    out[i * 2] = kHexUpper[b >> 4];
    out[i * 2 + 1] = kHexUpper[b & 0x0F];
  }
  out[12] = '\0';
  return std::string(out);
}

const std::string& ServerCredentialStore::ensureToken() {
  if (!token.empty()) return token;
  // 32 bytes del generador de hardware (esp_random se alimenta del ruido de la
  // radio, no de una semilla previsible).
  static const char kHexLower[] = "0123456789abcdef";
  std::string t;
  t.reserve(64);
  for (int i = 0; i < 8; ++i) {
    const uint32_t r = esp_random();
    for (int b = 3; b >= 0; --b) {
      const uint8_t v = static_cast<uint8_t>((r >> (8 * b)) & 0xFF);
      t += kHexLower[v >> 4];
      t += kHexLower[v & 0x0F];
    }
  }
  token = t;
  saveToFile();
  return token;
}
