#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

// Where the device's own server (the ws397 Hono backend) lives and the token
// it authenticates with. The token is the device's, never an Anthropic key:
// everything that needs one runs server-side. Persisted on the SD card like
// the KOReader credentials (token XOR-obfuscated with the eFuse MAC).
//
// serverUrl empty = derive the origin from the OTA release URL the build was
// given (CROSSPOINT_OTA_RELEASE_URL), so a ws397 build points at its server
// out of the box and only the token has to be entered (web UI > Server).
class ServerCredentialStore : public PersistableStore<ServerCredentialStore> {
 private:
  std::string serverUrl;
  std::string token;

  ServerCredentialStore() = default;
  ~ServerCredentialStore() = default;
  friend class PersistableStore<ServerCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/server.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  // Origin to prefix request paths with, without a trailing slash. Empty when
  // neither a custom URL nor a build-time OTA URL is available.
  std::string getBaseUrl() const;

  void setToken(const std::string& t);
  const std::string& getToken() const { return token; }
  bool hasToken() const { return !token.empty(); }
};

#define SERVER_STORE ServerCredentialStore::getInstance()
