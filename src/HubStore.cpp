#include "HubStore.h"

#include <time.h>

namespace {
std::string str(JsonVariantConst v, const char* key) {
  const char* s = v[key] | "";
  return std::string(s);
}

void parseReminders(JsonVariantConst doc, std::vector<HubStore::Reminder>& out) {
  out.clear();
  for (JsonVariantConst r : doc["reminders"].as<JsonArrayConst>()) {
    if (out.size() >= HubStore::MAX_REMINDERS) break;
    HubStore::Reminder rem;
    rem.id = r["id"] | 0;
    rem.title = str(r, "title");
    rem.when = str(r, "when");
    rem.dueAt = static_cast<time_t>(r["dueAt"] | (int64_t)0);
    rem.repeatText = str(r, "repeatText");
    rem.repeat = str(r, "repeat");
    rem.weekday = r["weekday"] | -1;
    rem.interval = r["interval"] | 0;
    out.push_back(std::move(rem));
  }
}

void parseNotes(JsonVariantConst doc, std::vector<HubStore::Note>& out) {
  out.clear();
  for (JsonVariantConst n : doc["notes"].as<JsonArrayConst>()) {
    if (out.size() >= HubStore::MAX_NOTES) break;
    out.push_back({n["id"] | 0, str(n, "text")});
  }
}

void parseLists(JsonVariantConst doc, std::vector<HubStore::List>& out) {
  out.clear();
  for (JsonVariantConst l : doc["lists"].as<JsonArrayConst>()) {
    if (out.size() >= HubStore::MAX_LISTS) break;
    HubStore::List list;
    list.key = str(l, "key");
    list.name = str(l, "name");
    if (list.key.empty()) list.key = list.name;   // servidor viejo: una sola clave
    if (list.name.empty()) list.name = list.key;
    for (JsonVariantConst i : l["items"].as<JsonArrayConst>()) {
      if (list.items.size() >= HubStore::MAX_ITEMS) break;
      list.items.push_back({i["id"] | 0, str(i, "text")});
    }
    out.push_back(std::move(list));
  }
}
}  // namespace

void HubStore::toJson(JsonDocument& doc) const {
  doc["syncedAt"] = static_cast<int64_t>(syncedAt);
  doc["lastAttemptAt"] = static_cast<int64_t>(lastAttemptAt);
  doc["weatherLine"] = weatherLine;
  doc["weatherDetail"] = weatherDetail;
  doc["weatherNoPlace"] = weatherNoPlace;
  doc["weatherError"] = weatherError;
  doc["reminderTitle"] = reminderTitle;
  doc["reminderWhen"] = reminderWhen;
  JsonArray rem = doc["reminders"].to<JsonArray>();
  for (const Reminder& r : reminders) {
    JsonObject o = rem.add<JsonObject>();
    o["id"] = r.id;
    o["title"] = r.title;
    o["when"] = r.when;
    o["dueAt"] = static_cast<int64_t>(r.dueAt);
    o["repeatText"] = r.repeatText;
    o["repeat"] = r.repeat;
    o["weekday"] = r.weekday;
    o["interval"] = r.interval;
  }
  JsonArray ls = doc["lists"].to<JsonArray>();
  for (const List& l : lists) {
    JsonObject o = ls.add<JsonObject>();
    o["key"] = l.key;
    o["name"] = l.name;
    JsonArray items = o["items"].to<JsonArray>();
    for (const ListItem& i : l.items) {
      JsonObject io = items.add<JsonObject>();
      io["id"] = i.id;
      io["text"] = i.text;
    }
  }
  JsonArray ns = doc["notes"].to<JsonArray>();
  for (const Note& n : notes) {
    JsonObject o = ns.add<JsonObject>();
    o["id"] = n.id;
    o["text"] = n.text;
  }
  JsonArray ev = doc["events"].to<JsonArray>();
  for (const Event& e : events) {
    JsonObject o = ev.add<JsonObject>();
    o["when"] = e.when;
    o["title"] = e.title;
  }
  doc["quote"] = quote;
  doc["verseRef"] = verseRef;
  doc["verseText"] = verseText;
  doc["translatorLang"] = translatorLang;
  doc["speakMode"] = speakMode;
  doc["uiSoundMode"] = uiSoundMode;
  doc["bibleBook"] = bibleBook;
  doc["bibleChapter"] = bibleChapter;
  doc["musicVolume"] = musicVolume;
  doc["motionGestures"] = motionGestures;
  if (imuMap.calibrated) {
    JsonObject m = doc["imuMap"].to<JsonObject>();
    m["na"] = imuMap.normalAxis;
    m["ns"] = imuMap.normalSign;
    m["xa"] = imuMap.xAxis;
    m["xs"] = imuMap.xSign;
    m["ya"] = imuMap.yAxis;
    m["ys"] = imuMap.ySign;
  }
  doc["wallpaperPath"] = wallpaperPath;
  doc["wallpaperName"] = wallpaperName;
  doc["timerEndAt"] = static_cast<int64_t>(timerEndAt);
  doc["timerTotal"] = timerTotal;
  doc["timerMode"] = timerMode;
  doc["timerPausedLeft"] = timerPausedLeft;
  doc["timerRound"] = timerRound;
  doc["stopwatchStartAt"] = static_cast<int64_t>(stopwatchStartAt);
  doc["stopwatchAccumS"] = stopwatchAccumS;
  doc["assetsVersion"] = assetsVersion;
  doc["assetsLang"] = assetsLang;
  doc["assetsFiles"] = assetsFiles;
  doc["assetsPending"] = assetsPending;
  doc["settingsRev"] = settingsRev;
  doc["account"] = account;
  doc["uiLang"] = uiLang;
  doc["ttsVoice"] = ttsVoice;
}

bool HubStore::fromJson(JsonVariantConst doc) {
  syncedAt = static_cast<time_t>(doc["syncedAt"] | (int64_t)0);
  lastAttemptAt = static_cast<time_t>(doc["lastAttemptAt"] | (int64_t)0);
  weatherLine = str(doc, "weatherLine");
  weatherDetail = str(doc, "weatherDetail");
  weatherNoPlace = doc["weatherNoPlace"] | false;
  weatherError = str(doc, "weatherError");
  reminderTitle = str(doc, "reminderTitle");
  reminderWhen = str(doc, "reminderWhen");
  parseReminders(doc, reminders);
  parseLists(doc, lists);
  parseNotes(doc, notes);
  events.clear();
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>()) {
    if (events.size() >= MAX_EVENTS) break;
    events.push_back({str(e, "when"), str(e, "title")});
  }
  quote = str(doc, "quote");
  verseRef = str(doc, "verseRef");
  verseText = str(doc, "verseText");
  translatorLang = str(doc, "translatorLang");
  speakMode = doc["speakMode"] | 1;
  uiSoundMode = doc["uiSoundMode"] | 0;
  bibleBook = doc["bibleBook"] | 0;
  bibleChapter = doc["bibleChapter"] | 0;
  musicVolume = doc["musicVolume"] | 70;
  motionGestures = doc["motionGestures"] | true;
  JsonVariantConst m = doc["imuMap"];
  if (!m.isNull()) {
    imuMap.normalAxis = m["na"] | 2;
    imuMap.normalSign = m["ns"] | 1;
    imuMap.xAxis = m["xa"] | 0;
    imuMap.xSign = m["xs"] | 1;
    imuMap.yAxis = m["ya"] | 1;
    imuMap.ySign = m["ys"] | 1;
    imuMap.calibrated = true;
    // Un mapa con dos ejes repetidos (archivo viejo o a mano) haría que dos
    // direcciones de la pantalla lean el mismo número: se descarta entero.
    if (imuMap.normalAxis > 2 || imuMap.xAxis > 2 || imuMap.yAxis > 2 || imuMap.normalAxis == imuMap.xAxis ||
        imuMap.normalAxis == imuMap.yAxis || imuMap.xAxis == imuMap.yAxis) {
      imuMap = ImuMap{};
    }
  }
  wallpaperPath = str(doc, "wallpaperPath");
  wallpaperName = str(doc, "wallpaperName");
  timerEndAt = static_cast<time_t>(doc["timerEndAt"] | (int64_t)0);
  timerTotal = doc["timerTotal"] | 0;
  timerMode = doc["timerMode"] | 0;
  timerPausedLeft = doc["timerPausedLeft"] | 0;
  timerRound = doc["timerRound"] | 1;
  stopwatchStartAt = static_cast<time_t>(doc["stopwatchStartAt"] | (int64_t)0);
  stopwatchAccumS = doc["stopwatchAccumS"] | 0;
  assetsVersion = str(doc, "assetsVersion");
  assetsLang = str(doc, "assetsLang");
  assetsFiles = doc["assetsFiles"] | 0;
  assetsPending = doc["assetsPending"] | false;
  settingsRev = doc["settingsRev"] | 0;
  account = str(doc, "account");
  uiLang = str(doc, "uiLang");
  ttsVoice = str(doc, "ttsVoice");
  return true;
}

// Server shape (see paper/src/hub.ts):
//   { ok, now, weather: {line, detail}, reminders: [{title, when}], events: [{when, title}],
//     quote }
void HubStore::applyServer(JsonVariantConst doc) {
  weatherLine = str(doc["weather"], "line");
  weatherDetail = str(doc["weather"], "detail");
  weatherNoPlace = doc["weather"]["noPlace"] | false;
  weatherError = str(doc["weather"], "error");
  parseReminders(doc, reminders);
  parseLists(doc, lists);
  parseNotes(doc, notes);
  reminderTitle = reminders.empty() ? "" : reminders[0].title;
  reminderWhen = reminders.empty() ? "" : reminders[0].when;
  events.clear();
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>()) {
    if (events.size() >= MAX_EVENTS) break;
    events.push_back({str(e, "when"), str(e, "title")});
  }
  quote = str(doc, "quote");
  verseRef = str(doc["verse"], "ref");
  verseText = str(doc["verse"], "text");
  applySettings(doc["settings"]);
  // Voz del servidor: si cambió, los clips cacheados en la SD son de la voz
  // vieja y hay que tirarlos (si no, el aviso del temporizador sigue sonando
  // con la voz que ya no se usa).
  const std::string voice = str(doc, "ttsVoice");
  if (!voice.empty()) ttsVoice = voice;
}

// { rev, lang, speak: "none"|"short"|"all", uiSound: "off"|"soft"|"normal", musicVolume, translatorLang }
void HubStore::applySettings(JsonVariantConst s) {
  if (s.isNull()) return;
  const int rev = s["rev"] | 0;
  if (rev <= settingsRev) return;
  settingsRev = rev;
  const std::string speak = str(s, "speak");
  if (speak == "none") speakMode = 0;
  else if (speak == "all") speakMode = 2;
  else if (speak == "short") speakMode = 1;
  // Sonidos de la interfaz, igual que los demás ajustes de la web: solo se
  // toca si la clave viene, así el que no la manda no los apaga.
  const std::string uiSound = str(s, "uiSound");
  if (uiSound == "off") uiSoundMode = 0;
  else if (uiSound == "soft") uiSoundMode = 1;
  else if (uiSound == "normal") uiSoundMode = 2;
  const int vol = s["musicVolume"] | -1;
  if (vol >= 0 && vol <= 100) musicVolume = vol;
  const std::string other = str(s, "translatorLang");
  if (!other.empty()) translatorLang = other;
  uiLang = str(s, "lang");
}

namespace {

// Dias desde 1970-01-01 (Howard Hinnant), la misma cuenta que usa HalClock: no
// depende de timegm() ni del huso del proceso.
long daysFromCivil(int y, const int m, const int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + static_cast<long>(doe) - 719468L;
}

int daysInMonth(const int y, const int m) {
  static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return DAYS[m - 1];
}

// La proxima ocurrencia de un recordatorio que repite, calculada EN EL APARATO.
// Es una aproximacion deliberada: el servidor es el que manda (nextOccurrence()
// en server/src/store.ts sabe de "hasta", de varios dias por semana y de los
// meses cortos), y la proxima sincronizacion pisa esto. Alcanza para que la
// alarma siga sonando mientras no hay WiFi, que es de lo que se trata.
time_t nextRepeatDue(const time_t due, const std::string& repeat, const int interval) {
  if (due <= 0 || repeat.empty() || repeat == "once") return 0;
  const long day = 86400L;
  if (repeat == "daily") return due + day;
  if (repeat == "weekdays") {
    // 0 = domingo en gmtime; saltear sabado y domingo.
    time_t next = due + day;
    for (int i = 0; i < 7; i++) {
      struct tm t = {};
      gmtime_r(&next, &t);
      if (t.tm_wday != 0 && t.tm_wday != 6) break;
      next += day;
    }
    return next;
  }
  if (repeat == "weekly") return due + 7 * day;
  if (repeat == "weeks") return due + 7L * day * (interval > 0 ? interval : 1);
  if (repeat == "monthly" || repeat == "yearly") {
    struct tm t = {};
    gmtime_r(&due, &t);
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    if (repeat == "monthly") {
      if (++m > 12) {
        m = 1;
        y++;
      }
    } else {
      y++;
    }
    const int d = t.tm_mday <= daysInMonth(y, m) ? t.tm_mday : daysInMonth(y, m);
    return static_cast<time_t>(daysFromCivil(y, m, d)) * day + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
  }
  return 0;
}

}  // namespace

bool HubStore::completeReminder(const int id, const time_t now) {
  for (Reminder& r : reminders) {
    if (r.id != id) continue;
    time_t next = nextRepeatDue(r.dueAt, r.repeat, r.interval);
    // Un diario que estuvo cuatro dias sin confirmarse: correrlo un solo paso
    // lo dejaria vencido y volveria a sonar en el acto, cuatro veces. Se corre
    // hasta pasar la hora actual (tope de 400 pasos: un anual no da mas de eso
    // ni con el reloj perdido).
    for (int i = 0; next > 0 && now > 0 && next <= now && i < 400; i++) {
      const time_t step = nextRepeatDue(next, r.repeat, r.interval);
      if (step <= next) break;
      next = step;
    }
    if (next > 0) {
      r.dueAt = next;
      // `when` viene traducido y armado por el servidor ("hoy 08:00"), asi que
      // aca queda viejo a proposito: no hay forma de rearmarlo sin duplicar el
      // formateo del servidor, y la proxima sincronizacion lo corrige. Lo que
      // importa es que el recordatorio siga existiendo y vuelva a sonar.
      if (!reminders.empty()) {
        reminderTitle = reminders[0].title;
        reminderWhen = reminders[0].when;
      }
      return true;
    }
    break;
  }
  removeReminder(id);
  return false;
}

void HubStore::clearAccountContent() {
  weatherLine.clear();
  weatherDetail.clear();
  weatherNoPlace = false;
  weatherError.clear();
  reminderTitle.clear();
  reminderWhen.clear();
  reminders.clear();
  lists.clear();
  notes.clear();
  events.clear();
  quote.clear();
  verseRef.clear();
  verseText.clear();
  // Los ajustes de /board son de la cuenta: con rev en 0, los de la cuenta
  // nueva se aplican aunque su numero de revision sea mas bajo que el viejo.
  settingsRev = 0;
  syncedAt = 0;
  lastAttemptAt = 0;
}

void HubStore::removeReminder(const int id) {
  for (auto it = reminders.begin(); it != reminders.end(); ++it) {
    if (it->id == id) {
      reminders.erase(it);
      break;
    }
  }
  reminderTitle = reminders.empty() ? "" : reminders[0].title;
  reminderWhen = reminders.empty() ? "" : reminders[0].when;
}

void HubStore::removeItem(const int id) {
  for (List& l : lists) {
    for (auto it = l.items.begin(); it != l.items.end(); ++it) {
      if (it->id == id) {
        l.items.erase(it);
        return;
      }
    }
  }
}

time_t HubStore::nextDueAt(const time_t now) const {
  time_t best = 0;
  for (const Reminder& r : reminders) {
    if (r.dueAt > now && (best == 0 || r.dueAt < best)) best = r.dueAt;
  }
  return best;
}

const HubStore::Reminder* HubStore::dueReminder(const time_t now) const {
  const Reminder* best = nullptr;
  for (const Reminder& r : reminders) {
    if (r.dueAt > 0 && r.dueAt <= now && (!best || r.dueAt < best->dueAt)) best = &r;
  }
  return best;
}

void HubStore::snoozeReminder(const int id, const time_t until) {
  for (Reminder& r : reminders) {
    if (r.id == id) r.dueAt = until;
  }
}

void HubStore::removeNote(const int id) {
  for (auto it = notes.begin(); it != notes.end(); ++it) {
    if (it->id == id) {
      notes.erase(it);
      return;
    }
  }
}

void HubStore::moveItem(const int id, const std::string& listKey) {
  ListItem moved;
  bool found = false;
  for (List& l : lists) {
    for (auto it = l.items.begin(); it != l.items.end(); ++it) {
      if (it->id == id) {
        moved = *it;
        l.items.erase(it);
        found = true;
        break;
      }
    }
    if (found) break;
  }
  if (!found) return;
  // `listKey` es la clave canónica del servidor. Ya no se crean listas nuevas
  // desde el aparato (el servidor tiene sólo compras y tareas): si la clave no
  // está, el ítem se queda donde estaba y lo acomoda la próxima sincronización.
  for (List& l : lists) {
    if (l.key == listKey) {
      l.items.push_back(moved);
      return;
    }
  }
}
