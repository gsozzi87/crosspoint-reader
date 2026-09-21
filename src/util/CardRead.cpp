#include "CardRead.h"

#include <HalStorage.h>
#include <Logging.h>

namespace cardread {

std::string readCapped(const char* tag, const std::string& path, const size_t cap) {
  HalFile f;
  if (!Storage.openFileForRead(tag, path, f)) return {};
  // `fileSize64()` y no `size()`: ver el comentario de `judge()`.
  const uint64_t size = f.fileSize64();
  const Verdict verdict = judge(size, cap);
  if (verdict != Verdict::Ok) {
    f.close();
    if (verdict == Verdict::TooBig) {
      LOG_ERR(tag, "«%s» mide %llu bytes y el tope son %u: se ignora (caché inválida)", path.c_str(),
              static_cast<unsigned long long>(size), static_cast<unsigned>(cap));
    }
    return {};
  }
  std::string out;
  out.resize(static_cast<size_t>(size));
  const int got = f.read(&out[0], out.size());
  f.close();
  if (got <= 0) return {};
  out.resize(static_cast<size_t>(got));
  return out;
}

}  // namespace cardread
