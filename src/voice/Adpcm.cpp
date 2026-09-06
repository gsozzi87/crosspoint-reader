#include "Adpcm.h"

#include <esp_heap_caps.h>

#include <cstring>

#include "util/WavHeader.h"

namespace {
const int16_t STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t INDEX_TABLE[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
}  // namespace

bool adpcm::isValid(const uint8_t* data, const size_t len) {
  if (!data || len < HEADER_BYTES) return false;
  if (memcmp(data, "ADPC", 4) != 0) return false;
  const uint32_t n = sampleCount(data, len);
  return n > 0 && HEADER_BYTES + (n + 1) / 2 <= len;
}

uint32_t adpcm::sampleCount(const uint8_t* data, const size_t len) {
  if (len < HEADER_BYTES) return 0;
  return static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
         (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
}

uint8_t* adpcm::decodeToWav(const uint8_t* data, const size_t len, size_t& wavBytes) {
  wavBytes = 0;
  if (!isValid(data, len)) return nullptr;
  const uint32_t n = sampleCount(data, len);
  const size_t bytes = wav::HEADER_BYTES + static_cast<size_t>(n) * sizeof(int16_t);
  uint8_t* out = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!out) out = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!out) return nullptr;
  int16_t* pcm = reinterpret_cast<int16_t*>(out + wav::HEADER_BYTES);
  int predictor = 0;
  int index = 0;
  int step = STEP_TABLE[0];
  const uint8_t* codes = data + HEADER_BYTES;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t byte = codes[i >> 1];
    const int code = (i & 1) ? (byte >> 4) : (byte & 15);
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor += (code & 8) ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;
    pcm[i] = static_cast<int16_t>(predictor);
    index += INDEX_TABLE[code];
    if (index < 0) index = 0;
    else if (index > 88) index = 88;
    step = STEP_TABLE[index];
  }
  wav::writeHeader(out, SAMPLE_RATE, static_cast<uint32_t>(n) * sizeof(int16_t));
  wavBytes = bytes;
  return out;
}
