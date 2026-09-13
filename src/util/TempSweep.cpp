#include "TempSweep.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <string>
#include <vector>

namespace {
constexpr const char* TAG = "TMPSWP";

// Dónde vive todo lo que el aparato escribe. No se barre la tarjeta entera a
// propósito: los libros y la música son del usuario y ahí no escribimos por
// este camino.
const char* DIRS[] = {"/.crosspoint", "/.crosspoint/rss", "/.crosspoint/att", "/.crosspoint/tts"};
}  // namespace

void tempsweep::run() {
  const std::string suffix = SDCardManager::TEMP_SUFFIX;
  int rescued = 0;
  int dropped = 0;
  for (const char* dir : DIRS) {
    if (!Storage.exists(dir)) continue;
    for (const String& name : Storage.listFiles(dir, 200)) {
      const std::string file = name.c_str();
      if (file.size() <= suffix.size() || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) {
        continue;
      }
      const std::string tmp = std::string(dir) + "/" + file;
      const std::string target = tmp.substr(0, tmp.size() - suffix.size());
      if (Storage.exists(target.c_str())) {
        // La escritura no llegó a confirmarse: lo bueno es lo que ya estaba.
        if (Storage.remove(tmp.c_str())) ++dropped;
      } else if (Storage.rename(tmp.c_str(), target.c_str())) {
        LOG_INF(TAG, "rescatado %s de una escritura cortada", target.c_str());
        ++rescued;
      }
    }
  }
  if (rescued > 0 || dropped > 0) {
    LOG_INF(TAG, "%d rescatados, %d sin confirmar descartados", rescued, dropped);
  }
}
