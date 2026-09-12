#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  // Validate an interrupted replacement before recovering it. Stream JSON:
  // readFile() is capped at 50 KB, but the queue and notes can be larger.
  const bool recovering = !Storage.exists(path);
  const std::string source = recovering ? std::string(path) + ".tmp" : path;
  if (!Storage.exists(source.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("PERSIST", source, file)) return false;
  struct Reader {
    HalFile& file;
    int read() { return file.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int count = file.read(buffer, length);
      return count > 0 ? static_cast<size_t>(count) : 0;
    }
  } reader{file};
  const auto error = deserializeJson(doc, reader);
  file.close();
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", source.c_str(), error.c_str());
    return false;
  }
  if (recovering && !Storage.rename(source.c_str(), path)) {
    LOG_ERR("PERSIST", "Could not recover %s", path);
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
