#pragma once

#include <cstdint>
#include <cstring>

// 44-byte canonical RIFF/WAVE header for 16-bit PCM mono, written in place.
// Shared by everything that captures from AudioManager (audio test, voice
// questions) so the layout is defined once.
namespace wav {

constexpr size_t HEADER_BYTES = 44;

inline void putLE32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
inline void putLE16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

inline void writeHeader(uint8_t* h, uint32_t sampleRate, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);
  putLE32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  putLE32(h + 16, 16);  // fmt chunk size
  putLE16(h + 20, 1);   // PCM
  putLE16(h + 22, 1);   // mono
  putLE32(h + 24, sampleRate);
  putLE32(h + 28, sampleRate * 2);  // byte rate
  putLE16(h + 32, 2);               // block align
  putLE16(h + 34, 16);              // bits per sample
  memcpy(h + 36, "data", 4);
  putLE32(h + 40, dataBytes);
}

}  // namespace wav
