#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
// Los idiomas del producto de esta placa son seis: español, inglés, francés,
// alemán, portugués y ruso. Las tablas de guiones de los otros cinco —finés,
// italiano, polaco, sueco, ucraniano— son 62 KB de flash que no se usan nunca.
// Se dejan afuera con un flag del entorno (`-DHYPH_PRODUCT_LANGS=1` en el env
// ws397) en vez de borrarlas: en las demás placas el juego completo sigue.
//
// El alemán solo pesa 201 KB, más que todos los demás juntos. Ese es el que
// tiene sentido mover a la tarjeta, no estos.
#include "generated/hyph-de.trie.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-ru.trie.h"
#if !HYPH_PRODUCT_LANGS
#include "generated/hyph-fi.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"
#endif

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);
#if !HYPH_PRODUCT_LANGS
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator finnishHyphenator(fi_patterns, isLatinLetter, toLowerLatin);
#endif

#if HYPH_PRODUCT_LANGS
using EntryArray = std::array<LanguageEntry, 5>;
#else
using EntryArray = std::array<LanguageEntry, 10>;
#endif

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", &frenchHyphenator},
                                       {"german", "de", &germanHyphenator},
                                       {"russian", "ru", &russianHyphenator},
                                       {"spanish", "es", &spanishHyphenator},
#if !HYPH_PRODUCT_LANGS
                                       {"italian", "it", &italianHyphenator},
                                       {"polish", "pl", &polishHyphenator},
                                       {"swedish", "sv", &swedishHyphenator},
                                       {"ukrainian", "uk", &ukrainianHyphenator},
                                       {"finnish", "fi", &finnishHyphenator},
#endif
                                       }};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
