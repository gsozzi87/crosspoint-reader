#include "SleepScreen.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "../../HubStore.h"
#include "../../RecentBooksStore.h"
#include "../../components/SevenSegment.h"
#include "../../CrossPointSettings.h"
#include "../../components/UITheme.h"
#include "../../util/Shtc3.h"
#include "../ListStyle.h"

extern GfxRenderer renderer;
extern HalClock halClock;

namespace {
constexpr const char* TAG = "SLEEPSCR";
constexpr const char* RSS_CACHE = "/.crosspoint/rss/feeds.json";

constexpr int M = listui::SIDE;          // el único margen lateral, 24
constexpr int FOOT_H = 84;               // banda de abajo: batería y cómo volver
constexpr int SEG_H = 54;                // alto de los dígitos de la alarma
constexpr int SEG_W = 30;
constexpr int SEG_T = 6;
constexpr int SEG_GAP = 10;

int right() { return renderer.getScreenWidth() - M; }
int contentW() { return renderer.getScreenWidth() - 2 * M; }
int footTop() { return renderer.getScreenHeight() - FOOT_H; }

// Versalita espaciada: el antetítulo del sistema visual. Nunca una pastilla
// negra con texto blanco (DISENO.md: eso dejaba fantasma y saltos de contraste).
void spaced(const int fontId, const int x, const int y, const std::string& text, const int gap) {
  std::string up;
  up.reserve(text.size());
  // Sólo ASCII: los acentos son multibyte y tocarlos rompería el UTF-8. Las
  // etiquetas de esta pantalla son nuestras y ya vienen en mayúsculas.
  for (const char c : text) up.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
  int cx = x;
  char one[5] = {0};
  for (size_t i = 0; i < up.size();) {
    const unsigned char lead = static_cast<unsigned char>(up[i]);
    const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x06 ? 2 : (lead >> 4) == 0x0E ? 3 : 4;
    const size_t n = std::min(len, up.size() - i);
    memcpy(one, up.data() + i, n);
    one[n] = 0;
    renderer.drawText(fontId, cx, y, one, true, EpdFontFamily::BOLD);
    cx += renderer.getTextWidth(fontId, one, EpdFontFamily::BOLD) + gap;
    i += n;
  }
}

void rule(const int y, const int weight = 1) { renderer.fillRect(M, y, contentW(), weight, true); }

// Corta en palabras contra el ancho real. `maxLines` renglones como mucho; lo
// que sobra se resume en el último con puntos suspensivos.
std::vector<std::string> wrap(const int fontId, const std::string& text, const int maxW, const size_t maxLines) {
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i <= text.size()) {
    const size_t sp = text.find(' ', i);
    const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
    const std::string probe = line.empty() ? word : line + " " + word;
    if (renderer.getTextWidth(fontId, probe.c_str()) <= maxW) {
      line = probe;
    } else {
      if (!line.empty()) out.push_back(line);
      line = word;
      if (out.size() == maxLines) break;
    }
    if (sp == std::string::npos) break;
    i = sp + 1;
  }
  if (out.size() < maxLines && !line.empty()) out.push_back(line);
  if (out.size() == maxLines && !line.empty() && out.back() != line) {
    out.back() = renderer.truncatedText(fontId, (out.back() + " " + line).c_str(), maxW);
  }
  return out;
}

// "SUSPENDIDO · 21:53 · viernes 11 de septiembre" repartido en dos renglones.
int header(const sleepscreen::State state) {
  const StrId word = state == sleepscreen::State::Suspended ? StrId::STR_SLEEPSCR_SUSPENDED : StrId::STR_SLEEPSCR_OFF;
  spaced(UI_14_FONT_ID, M, 16, I18N.get(word), 6);

  char stamp[32] = {0};
  if (halClock.isAvailable()) {
    halClock.formatTime(stamp, sizeof(stamp), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);
  }
  if (stamp[0] != 0) renderer.drawText(SMALL_FONT_ID, M, 58, stamp, true, EpdFontFamily::BOLD);
  rule(88, 3);
  rule(94, 1);
  return 112;
}

// La próxima alarma, en dígitos de segmentos: es el ÚNICO dato de esta pantalla
// que sigue siendo cierto mientras el aparato duerme.
int nextAlarm(int y) {
  time_t now = 0;
  const bool haveClock = halClock.getEpochUtc(now) && now > 1600000000;
  const HubStore::Reminder* next = nullptr;
  time_t at = 0;
  for (const auto& r : HUB_STORE.reminders) {
    if (r.dueAt <= 0) continue;
    if (haveClock && r.dueAt < now) continue;
    if (at == 0 || r.dueAt < at) {
      at = r.dueAt;
      next = &r;
    }
  }
  if (next == nullptr) {
    spaced(SMALL_FONT_ID, M, y, I18N.get(StrId::STR_SLEEPSCR_NO_ALARM), 3);
    return y + 30;
  }

  spaced(SMALL_FONT_ID, M, y, I18N.get(StrId::STR_SLEEPSCR_NEXT_ALARM), 3);
  y += 26;
  // clockUtcOffsetQ son cuartos de hora con sesgo 48 (48 = UTC+0).
  const long offsetS = (static_cast<long>(SETTINGS.clockUtcOffsetQ) - 48L) * 900L;
  const time_t local = at + offsetS;
  struct tm tmv;
  gmtime_r(&local, &tmv);
  int x = M;
  const int hh = tmv.tm_hour, mm = tmv.tm_min;
  sevenseg::digit(renderer, hh / 10, x, y, SEG_W, SEG_H, SEG_T);
  x += SEG_W + SEG_GAP;
  sevenseg::digit(renderer, hh % 10, x, y, SEG_W, SEG_H, SEG_T);
  x += SEG_W + SEG_GAP;
  sevenseg::colon(renderer, x, y, SEG_W / 2, SEG_H, SEG_T);
  x += SEG_W / 2 + SEG_GAP;
  sevenseg::digit(renderer, mm / 10, x, y, SEG_W, SEG_H, SEG_T);
  x += SEG_W + SEG_GAP;
  sevenseg::digit(renderer, mm % 10, x, y, SEG_W, SEG_H, SEG_T);
  x += SEG_W + 24;

  if (!next->when.empty()) {
    renderer.drawText(SMALL_FONT_ID, x, y + 6, renderer.truncatedText(SMALL_FONT_ID, next->when.c_str(), right() - x).c_str());
  }
  y += SEG_H + 8;
  renderer.drawText(UI_12_FONT_ID, M, y,
                    renderer.truncatedText(UI_12_FONT_ID, next->title.c_str(), contentW()).c_str());
  return y + 40;
}

int weather(int y) {
  if (HUB_STORE.weatherLine.empty()) return y;
  renderer.drawText(UI_14_FONT_ID, M, y, HUB_STORE.weatherLine.c_str(), true, EpdFontFamily::BOLD);
  const float inside = shtc3::cachedCelsius();
  if (!std::isnan(inside)) {
    char in[48];
    snprintf(in, sizeof(in), "%s %d°", I18N.get(StrId::STR_HUB_INDOOR), static_cast<int>(inside + 0.5f));
    const int w = renderer.getTextWidth(SMALL_FONT_ID, in, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, right() - w, y + 6, in, true, EpdFontFamily::BOLD);
  }
  y += renderer.getLineHeight(UI_14_FONT_ID) + 6;
  if (!HUB_STORE.weatherDetail.empty()) {
    renderer.drawText(SMALL_FONT_ID, M, y,
                      renderer.truncatedText(SMALL_FONT_ID, HUB_STORE.weatherDetail.c_str(), contentW()).c_str());
    y += 26;
  }
  return y + 8;
}

int book(int y) {
  const auto& books = RECENT_BOOKS.getBooks();
  if (books.empty()) return y;
  spaced(SMALL_FONT_ID, M, y, I18N.get(StrId::STR_SLEEPSCR_READING), 3);
  y += 26;
  renderer.drawText(UI_12_FONT_ID, M, y,
                    renderer.truncatedText(UI_12_FONT_ID, books.front().title.c_str(), contentW()).c_str());
  y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  if (!books.front().author.empty()) {
    renderer.drawText(SMALL_FONT_ID, M, y,
                      renderer.truncatedText(SMALL_FONT_ID, books.front().author.c_str(), contentW()).c_str());
    y += 24;
  }
  return y + 8;
}

// Los que entren entre `y` y el pie, y se corta limpio: nunca a mitad de
// renglón. Devuelve cuántos puso.
size_t headlines(int y, const std::vector<std::string>& list) {
  if (list.empty()) return 0;
  const int tope = footTop() - 12;
  if (y + 60 > tope) return 0;
  spaced(SMALL_FONT_ID, M, y, I18N.get(StrId::STR_SLEEPSCR_HEADLINES), 3);
  y += 28;
  size_t n = 0;
  for (const std::string& item : list) {
    const auto lines = wrap(UI_12_FONT_ID, item, contentW(), 2);
    const int h = static_cast<int>(lines.size()) * (renderer.getLineHeight(UI_12_FONT_ID) + 2) + 16;
    if (y + h > tope) break;
    if (n > 0) rule(y - 10);
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, M, y, line.c_str());
      y += renderer.getLineHeight(UI_12_FONT_ID) + 2;
    }
    y += 16;
    ++n;
  }
  return n;
}

void foot(const sleepscreen::State state) {
  const int y = footTop();
  rule(y, 2);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawBatteryLeft(renderer, Rect{M, y + 18, metrics.batteryWidth, metrics.batteryHeight}, true);
  const char* how = I18N.get(state == sleepscreen::State::Suspended ? StrId::STR_SLEEPSCR_WAKE_OK
                                                                   : StrId::STR_SLEEPSCR_WAKE_PWR);
  const int w = renderer.getTextWidth(SMALL_FONT_ID, how, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, right() - w, y + 24, how, true, EpdFontFamily::BOLD);
}
}  // namespace

std::vector<std::string> sleepscreen::readHeadlines(const int max) {
  std::vector<std::string> out;
  if (!Storage.exists(RSS_CACHE)) return out;
  HalFile f;
  if (!Storage.openFileForRead(TAG, RSS_CACHE, f)) return out;
  std::string raw;
  raw.resize(f.size());
  const int got = raw.empty() ? 0 : f.read(reinterpret_cast<uint8_t*>(&raw[0]), raw.size());
  f.close();
  if (got <= 0) return out;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return out;
  // Uno por feed y después la segunda vuelta: así la pantalla no queda con
  // cinco titulares del mismo diario.
  for (int round = 0; round < 3 && static_cast<int>(out.size()) < max; ++round) {
    for (JsonVariantConst fv : doc["feeds"].as<JsonArrayConst>()) {
      JsonArrayConst items = fv["items"].as<JsonArrayConst>();
      if (static_cast<int>(items.size()) <= round) continue;
      const char* t = items[round]["title"];
      if (t == nullptr || *t == 0) continue;
      out.emplace_back(t);
      if (static_cast<int>(out.size()) >= max) break;
    }
  }
  return out;
}

bool sleepscreen::paint(GfxRenderer& r, const State state, const std::vector<std::string>& news) {
  renderer.clearScreen();
  int y = header(state);
  y = nextAlarm(y);
  rule(y);
  y = weather(y + 20);
  if (y < footTop() - 200) {
    rule(y);
    y = book(y + 20);
  }
  rule(y, 2);
  const size_t n = headlines(y + 18, news);
  foot(state);
  // FULL y no parcial: es la última pintura y va a quedar en el vidrio durante
  // horas o días. Un parcial deja fantasma de lo que había antes, y acá no hay
  // ninguna pintura posterior que lo limpie.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  LOG_INF(TAG, "fondo de %s pintado (%u titulares)", state == State::Suspended ? "suspendido" : "apagado",
          static_cast<unsigned>(n));
  return true;
}
