#include "DeviceLog.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <ws397_version.h>

#include <cstring>

namespace {
constexpr const char* DIR = "/.crosspoint";
constexpr const char* CURRENT = "/.crosspoint/device.log";
constexpr const char* PREVIOUS = "/.crosspoint/device.prev.log";
constexpr size_t MAX_BYTES = 64 * 1024;
constexpr size_t FLUSH_EVERY = 2 * 1024;

HalFile file;
size_t written = 0;
size_t sinceFlush = 0;
bool ready = false;
bool inWrite = false;

void openCurrent(const bool append) {
  if (file.isOpen()) file.close();
  if (!append && Storage.exists(CURRENT)) {
    Storage.remove(PREVIOUS);
    Storage.rename(CURRENT, PREVIOUS);
  }
  HalFile f;
  if (!Storage.openFileForWrite("LOG", CURRENT, f)) return;
  if (append) f.seek(f.size());
  written = f.size();
  file = std::move(f);
}
}  // namespace

void devlog::begin() {
  if (ready) return;
  Storage.ensureDirectoryExists(DIR);
  openCurrent(true);
  ready = file.isOpen();
  if (!ready) return;
  char banner[96];
  snprintf(banner, sizeof(banner), "\n=== boot %s ===\n", WS397_VERSION);
  file.write(reinterpret_cast<const uint8_t*>(banner), strlen(banner));
  file.flush();
}

void devlog::write(const char* line) {
  if (!ready || !line || inWrite) return;
  inWrite = true;  // a failing write must not log itself
  char stamp[16] = "";
  uint8_t h = 0, m = 0;
  if (halClock.isAvailable() && halClock.getTime(h, m)) snprintf(stamp, sizeof(stamp), "%02u:%02u ", h, m);
  if (stamp[0]) file.write(reinterpret_cast<const uint8_t*>(stamp), strlen(stamp));
  const size_t len = strlen(line);
  file.write(reinterpret_cast<const uint8_t*>(line), len);
  written += len + strlen(stamp);
  sinceFlush += len;
  if (sinceFlush >= FLUSH_EVERY) {
    file.flush();
    sinceFlush = 0;
  }
  if (written >= MAX_BYTES) openCurrent(false);
  inWrite = false;
}

void devlog::flush() {
  if (ready && file.isOpen()) file.flush();
}

size_t devlog::size() { return written; }

std::string devlog::tail(const size_t maxBytes) {
  devlog::flush();
  std::string out;
  auto readInto = [&](const char* path) {
    HalFile f;
    if (!Storage.openFileForRead("LOG", path, f)) return;
    const size_t n = f.size();
    const size_t want = std::min(n, maxBytes - std::min(out.size(), maxBytes));
    if (want == 0) {
      f.close();
      return;
    }
    std::string chunk;
    chunk.resize(want);
    f.seek(n - want);
    const int got = f.read(&chunk[0], want);
    f.close();
    if (got > 0) {
      chunk.resize(got);
      out += chunk;
    }
  };
  if (Storage.exists(PREVIOUS) && maxBytes > 8 * 1024) readInto(PREVIOUS);
  readInto(CURRENT);
  return out;
}
