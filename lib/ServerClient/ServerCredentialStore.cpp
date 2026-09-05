#include "ServerCredentialStore.h"

#include <ObfuscationUtils.h>

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
