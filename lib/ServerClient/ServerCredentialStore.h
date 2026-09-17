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

  // Identidad pública del aparato: la MAC de fábrica en hex, sin separadores.
  // Es lo que se manda al vincularlo y lo que se ve en la web; no es secreta.
  static std::string deviceId();

  // El token propio del aparato. Se genera UNA sola vez, al azar, y se guarda
  // acá; después se vincula a una cuenta desde la web con el código que muestra
  // la pantalla.
  //
  // NO se deriva de la MAC. Derivarlo tendría la ventaja de recuperarlo solo
  // después de un borrado, pero para eso hay que meter un secreto de fábrica en
  // el firmware, y cualquiera que baje un .bin puede sacarlo y, con eso,
  // calcular el token de CUALQUIER aparato a partir de su MAC — que además va
  // impresa en la caja. Al azar no hay nada que deducir, y perder el token no
  // pierde datos: los datos son de la CUENTA, así que se vuelve a vincular con
  // el código de seis dígitos y listo.
  const std::string& ensureToken();

  // Acuñar A PEDIDO, aunque `ensureToken()` se haya negado. Es la salida del
  // callejón: si la tarjeta se dañó y server.json existe pero no se puede leer,
  // el arranque NO acuña (cambiar la identidad solo es peor que no tenerla),
  // pero el que entra a Ajustes -> Vincular con mi cuenta está diciendo
  // explícitamente "dale identidad nueva a este aparato y lo vinculo de nuevo".
  // Sin esto el aparato quedaba sin token, sin poder acuñar y sin poder
  // vincularse: el servidor rechaza un token vacío y no hay forma de salir.
  const std::string& mintToken();
};

#define SERVER_STORE ServerCredentialStore::getInstance()
