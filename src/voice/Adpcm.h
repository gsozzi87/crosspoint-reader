#pragma once

#include <cstddef>
#include <cstdint>

// IMA ADPCM as the server's tts.ts writes it: 8-byte header "ADPC" + sample
// count (uint32 LE), then 4-bit codes, low nibble first, one continuous run
// (no block headers), 16 kHz mono. 4x smaller than PCM; decoding is a few
// integer ops per sample, so a 4 s clip decodes in a couple of ms.
namespace adpcm {
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t HEADER_BYTES = 8;

bool isValid(const uint8_t* data, size_t len);
uint32_t sampleCount(const uint8_t* data, size_t len);
// Decodes into a complete WAV (header + 16-bit PCM) allocated with
// heap_caps_malloc (PSRAM first). Caller frees with heap_caps_free.
uint8_t* decodeToWav(const uint8_t* data, size_t len, size_t& wavBytes);
}  // namespace adpcm
