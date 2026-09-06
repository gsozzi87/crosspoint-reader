#pragma once

#include <I18n.h>

// Two-letter code of the UI language for the server (STT language, reply
// language, translator source). The server knows es, en, zh, fr, de, pt, ru
// and falls back to Spanish for anything else.
inline const char* uiLanguageCode() {
  switch (I18N.getLanguage()) {
    case Language::EN: return "en";
    case Language::ES: return "es";
    case Language::FR: return "fr";
    case Language::DE: return "de";
    case Language::PT: return "pt";
    case Language::P2: return "pt";
    case Language::RU: return "ru";
    default: return "en";
  }
}
