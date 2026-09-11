#include "Mp3Source.h"

#include <Logging.h>
#include <esp_heap_caps.h>
#include <mp3dec.h>

#include <cstring>

namespace {
constexpr const char* TAG = "MP3";

std::string decodeTextFrame(const uint8_t* p, size_t len) {
  if (len < 2) return "";
  const uint8_t enc = p[0];
  p++;
  len--;
  std::string out;
  if (enc == 0 || enc == 3) {  // Latin-1 (treated as-is) / UTF-8
    out.assign(reinterpret_cast<const char*>(p), len);
  } else {  // UTF-16 with BOM (1) or BE (2): keep the low bytes of BMP chars
    bool le = true;
    if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; len -= 2; }
    else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) { p += 2; len -= 2; le = false; }
    else if (enc == 2) le = false;
    for (size_t i = 0; i + 1 < len; i += 2) {
      const uint16_t cp = le ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
      if (cp == 0) break;
      if (cp < 0x80) out += static_cast<char>(cp);
      else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
  }
  while (!out.empty() && (out.back() == '\0' || out.back() == ' ')) out.pop_back();
  return out;
}
}  // namespace

bool Mp3Source::open(const std::string& path) {
  close();
  if (!Storage.openFileForRead(TAG, path, file_)) return false;
  fileSize_ = file_.size();
  inBuf_ = static_cast<uint8_t*>(heap_caps_malloc(IN_BUF, MALLOC_CAP_8BIT));
  pcmBuf_ = static_cast<uint8_t*>(heap_caps_malloc(PCM_BUF, MALLOC_CAP_8BIT));
  decoder_ = MP3InitDecoder();
  if (!inBuf_ || !pcmBuf_ || !decoder_) {
    LOG_ERR(TAG, "no memory for the decoder");
    close();
    return false;
  }
  parseId3v2();
  parseId3v1();
  file_.seek(audioStart_);
  inLen_ = 0;
  eof_ = false;
  // Decode one frame to learn the format; it is kept as the first PCM out.
  if (!decodeFrame()) {
    LOG_ERR(TAG, "no MP3 frame found in %s", path.c_str());
    close();
    return false;
  }
  if (bitrate_ > 0) duration_ = static_cast<int>(static_cast<uint64_t>(fileSize_ - audioStart_) * 8 / bitrate_);
  samplesOut_ = 0;
  buildHeader();
  headerPos_ = 0;
  inHeader_ = true;
  if (title_.empty()) {
    // Filename without folder and extension.
    const size_t slash = path.find_last_of('/');
    title_ = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = title_.find_last_of('.');
    if (dot != std::string::npos) title_.resize(dot);
  }
  LOG_INF(TAG, "%s: %d Hz %dch %d kbps ~%d s \"%s\" - \"%s\"", path.c_str(), sampleRate_, channels_, bitrate_ / 1000,
          duration_, artist_.c_str(), title_.c_str());
  return true;
}

void Mp3Source::close() {
  if (file_.isOpen()) file_.close();
  if (decoder_) {
    MP3FreeDecoder(static_cast<HMP3Decoder>(decoder_));
    decoder_ = nullptr;
  }
  if (inBuf_) { heap_caps_free(inBuf_); inBuf_ = nullptr; }
  if (pcmBuf_) { heap_caps_free(pcmBuf_); pcmBuf_ = nullptr; }
  inLen_ = pcmAvail_ = pcmPos_ = 0;
  audioStart_ = 0;
  title_.clear();
  artist_.clear();
  sampleRate_ = channels_ = bitrate_ = duration_ = 0;
  samplesOut_ = 0;
  for (uint8_t& l : levels_) l = 0;
  levelPos_ = 0;
  levelPeak_ = 0;
  levelFrames_ = 0;
}

// ID3v2: "ID3" vv f ssss (syncsafe). Frames "TIT2"/"TPE1" (v2.3/2.4) or "TT2"/"TP1" (v2.2).
void Mp3Source::parseId3v2() {
  uint8_t h[10];
  file_.seek(0);
  if (file_.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return;
  const uint8_t major = h[3];
  const size_t size = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
  audioStart_ = 10 + size + ((h[5] & 0x10) ? 10 : 0);
  size_t pos = 10;
  if (major >= 3 && (h[5] & 0x40)) {  // extended header
    uint8_t eh[4];
    if (file_.read(eh, 4) == 4) {
      const size_t ehSize = major == 4 ? (((eh[0] & 0x7F) << 21) | ((eh[1] & 0x7F) << 14) | ((eh[2] & 0x7F) << 7) | (eh[3] & 0x7F))
                                       : ((eh[0] << 24) | (eh[1] << 16) | (eh[2] << 8) | eh[3]) + 4;
      pos += ehSize;
    }
  }
  const size_t end = 10 + size;
  uint8_t buf[512];
  while (pos + 10 <= end && title_.empty() + artist_.empty() > 0) {
    file_.seek(pos);
    const int hdrLen = major == 2 ? 6 : 10;
    if (file_.read(buf, hdrLen) != hdrLen || buf[0] == 0) break;
    char id[5] = {0};
    memcpy(id, buf, major == 2 ? 3 : 4);
    size_t frameSize;
    if (major == 2) frameSize = (buf[3] << 16) | (buf[4] << 8) | buf[5];
    else if (major == 4) frameSize = ((buf[4] & 0x7F) << 21) | ((buf[5] & 0x7F) << 14) | ((buf[6] & 0x7F) << 7) | (buf[7] & 0x7F);
    else frameSize = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    pos += hdrLen;
    const bool isTitle = strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0;
    const bool isArtist = strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0;
    if ((isTitle || isArtist) && frameSize > 0 && frameSize <= sizeof(buf)) {
      const int got = file_.read(buf, frameSize);
      if (got > 0) {
        const std::string text = decodeTextFrame(buf, got);
        if (isTitle) title_ = text; else artist_ = text;
      }
    }
    pos += frameSize;
  }
}

void Mp3Source::parseId3v1() {
  if (!title_.empty() || fileSize_ < 128) return;
  uint8_t t[128];
  file_.seek(fileSize_ - 128);
  if (file_.read(t, 128) != 128 || memcmp(t, "TAG", 3) != 0) return;
  auto field = [&](int off, int len) {
    std::string s(reinterpret_cast<const char*>(t + off), len);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
  };
  title_ = field(3, 30);
  artist_ = field(33, 30);
  fileSize_ -= 128;  // keep the tag out of the duration estimate
}

bool Mp3Source::fillInput() {
  if (inLen_ >= IN_BUF) return true;
  const int n = file_.read(inBuf_ + inLen_, IN_BUF - inLen_);
  if (n <= 0) return inLen_ > 0;
  inLen_ += n;
  return true;
}

bool Mp3Source::decodeFrame() {
  for (int attempts = 0; attempts < 8; ++attempts) {
    if (!fillInput()) {
      eof_ = true;
      return false;
    }
    const int sync = MP3FindSyncWord(inBuf_, inLen_);
    if (sync < 0) {
      inLen_ = 0;  // junk: drop the buffer and read on
      continue;
    }
    if (sync > 0) {
      memmove(inBuf_, inBuf_ + sync, inLen_ - sync);
      inLen_ -= sync;
      if (!fillInput()) {
        eof_ = true;
        return false;
      }
    }
    unsigned char* p = inBuf_;
    int left = static_cast<int>(inLen_);
    const int err = MP3Decode(static_cast<HMP3Decoder>(decoder_), &p, &left, reinterpret_cast<short*>(pcmBuf_), 0);
    const size_t used = inLen_ - static_cast<size_t>(left);
    if (err == ERR_MP3_NONE) {
      MP3FrameInfo info;
      MP3GetLastFrameInfo(static_cast<HMP3Decoder>(decoder_), &info);
      if (info.outputSamps <= 0) continue;
      if (sampleRate_ == 0) {
        sampleRate_ = info.samprate;
        channels_ = info.nChans;
        bitrate_ = info.bitrate;
      }
      memmove(inBuf_, p, left);
      inLen_ = left;
      pcmAvail_ = static_cast<size_t>(info.outputSamps) * sizeof(int16_t);
      pcmPos_ = 0;
      // Pico del bloque para el analizador. Un cuadro son 1152 muestras (~26 ms),
      // así que se junta el pico de ocho y se guarda uno: ~5 barras por segundo,
      // 24 barras = los últimos cinco segundos.
      {
        const int16_t* s16 = reinterpret_cast<const int16_t*>(pcmBuf_);
        const int n = info.outputSamps;
        uint16_t peak = 0;
        for (int i = 0; i < n; i += 4) {  // de a cuatro: el pico no cambia y cuesta la cuarta parte
          const int16_t v = s16[i];
          const uint16_t a = v < 0 ? static_cast<uint16_t>(-(v + 1)) : static_cast<uint16_t>(v);
          if (a > peak) peak = a;
        }
        if (peak > levelPeak_) levelPeak_ = peak;
        if (++levelFrames_ >= 8) {
          // >> 11, no >> 12: una muestra de 16 bits llega hasta 32767 y con 12
          // el maximo posible daba 7 sobre 15, o sea que la barra NUNCA podia
          // pasar de media altura por mucho que sonara. Con 11 el fondo de
          // escala es el fondo de escala.
          const uint16_t scaled = static_cast<uint16_t>(levelPeak_ >> 11);
          levels_[levelPos_] = static_cast<uint8_t>(scaled > 15 ? 15 : scaled);
          levelPos_ = (levelPos_ + 1) % LEVELS;
          levelPeak_ = 0;
          levelFrames_ = 0;
        }
      }
      return true;
    }
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
      if (used == 0 && inLen_ >= IN_BUF) {  // cannot progress: skip a byte
        memmove(inBuf_, inBuf_ + 1, inLen_ - 1);
        inLen_--;
      } else if (used > 0) {
        memmove(inBuf_, p, left);
        inLen_ = left;
      }
      if (file_.available() <= 0 && inLen_ < 4) {
        eof_ = true;
        return false;
      }
      continue;
    }
    // Other errors: skip the bad sync and keep going.
    memmove(inBuf_, inBuf_ + 1, inLen_ - 1);
    inLen_--;
  }
  return false;
}

int Mp3Source::readPcm(uint8_t* dst, const size_t len) {
  size_t out = 0;
  while (out < len) {
    if (pcmPos_ >= pcmAvail_) {
      if (eof_ || !decodeFrame()) break;
    }
    const size_t n = std::min(len - out, pcmAvail_ - pcmPos_);
    memcpy(dst + out, pcmBuf_ + pcmPos_, n);
    pcmPos_ += n;
    out += n;
  }
  samplesOut_ += out / sizeof(int16_t) / (channels_ > 0 ? channels_ : 1);
  return static_cast<int>(out);
}

void Mp3Source::buildHeader() {
  const uint32_t rate = sampleRate_ > 0 ? sampleRate_ : 44100;
  const uint16_t ch = channels_ > 0 ? channels_ : 2;
  const uint32_t dataBytes = 0xFFFFFFF0u;  // "endless": the task stops when read() returns 0
  uint8_t* h = header_;
  memcpy(h, "RIFF", 4);
  h[4] = 0xFF; h[5] = 0xFF; h[6] = 0xFF; h[7] = 0xFF;
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16; h[17] = h[18] = h[19] = 0;
  h[20] = 1; h[21] = 0;
  h[22] = ch; h[23] = 0;
  h[24] = rate & 0xFF; h[25] = (rate >> 8) & 0xFF; h[26] = (rate >> 16) & 0xFF; h[27] = (rate >> 24) & 0xFF;
  const uint32_t byteRate = rate * ch * 2;
  h[28] = byteRate & 0xFF; h[29] = (byteRate >> 8) & 0xFF; h[30] = (byteRate >> 16) & 0xFF; h[31] = (byteRate >> 24) & 0xFF;
  h[32] = ch * 2; h[33] = 0;
  h[34] = 16; h[35] = 0;
  memcpy(h + 36, "data", 4);
  h[40] = dataBytes & 0xFF; h[41] = (dataBytes >> 8) & 0xFF; h[42] = (dataBytes >> 16) & 0xFF; h[43] = (dataBytes >> 24) & 0xFF;
}

AudioManager::WavSource Mp3Source::wavSource() {
  AudioManager::WavSource src;
  src.read = [this](uint8_t* dst, size_t len) -> int {
    if (inHeader_) {
      const size_t n = std::min(len, sizeof(header_) - headerPos_);
      memcpy(dst, header_ + headerPos_, n);
      headerPos_ += n;
      if (headerPos_ >= sizeof(header_)) inHeader_ = false;
      return static_cast<int>(n);
    }
    return readPcm(dst, len);
  };
  // POR QUE LA MUSICA NUNCA SONO (hasta 1.5.44): AudioManager::parseWavHeader no
  // lee la cabecera de corrido, la RECORRE por chunks. Hace seek(0), seek(12)
  // para el "fmt " y seek(36) para el "data" antes del seek(44) final. Este
  // lambda solo aceptaba 0 y 44, asi que el segundo seek devolvia false,
  // parseWavHeader cortaba, play() fallaba y no sonaba nada nunca. Ahora
  // cualquier posicion DENTRO de la cabecera sintetica vuelve a la cabecera, y
  // 44 pasa al PCM. Mas alla de eso sigue sin haber busqueda (el MP3 se decodifica
  // de corrido, no se puede saltar).
  src.seek = [this](size_t pos) -> bool {
    if (pos < sizeof(header_)) {
      inHeader_ = true;
      headerPos_ = pos;
      return true;
    }
    if (pos == sizeof(header_)) {  // dataStart: PCM begins with the frame decoded at open()
      inHeader_ = false;
      return true;
    }
    return false;  // no looping / scrubbing
  };
  return src;
}
