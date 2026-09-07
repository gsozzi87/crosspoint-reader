#include "SpeechCache.h"

#include <cstdint>
#include <cstdio>

#include "HubStore.h"
#include "voice/Lang.h"

namespace {
// FNV-1a de 32 bits: alcanza y sobra para separar un puñado de avisos.
uint32_t hash32(const std::string& s) {
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}
}  // namespace

std::string speechcache::clipPath(const std::string& text) {
  const std::string key = HUB_STORE.ttsVoice + "|" + uiLanguageCode() + "|" + text;
  char name[24];
  snprintf(name, sizeof(name), "/%08x.bin", static_cast<unsigned>(hash32(key)));
  return std::string(DIR) + name;
}
