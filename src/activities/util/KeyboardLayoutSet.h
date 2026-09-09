#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
};

// Table position is the persisted bit assignment. Keep existing rows in place
// and append new layouts so SDK enum changes cannot reinterpret saved masks.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, Language::FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, Language::DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, Language::ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, Language::RU},
    // Ucraniano, bielorruso, kazajo y hebreo no se compilan en este fork (los
    // idiomas del producto son siete), asi que esas distribuciones se ofrecen
    // en ruso, que comparte el alfabeto en tres de los cuatro casos. La POSICION
    // de cada fila es el bit guardado en disco: no se puede reordenar ni sacar.
    {freeink::ui::KeyboardLayoutId::CyrillicUk, Language::RU},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, Language::RU},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, Language::RU},
    {freeink::ui::KeyboardLayoutId::HebrewIl, Language::EN},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
// Symbol layers have no Latin letters, so credentials and URLs require at
// least one of these layouts to remain enabled.
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
