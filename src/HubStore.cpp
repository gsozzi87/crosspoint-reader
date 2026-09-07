#include "HubStore.h"

namespace {
std::string str(JsonVariantConst v, const char* key) {
  const char* s = v[key] | "";
  return std::string(s);
}

void parseReminders(JsonVariantConst doc, std::vector<HubStore::Reminder>& out) {
  out.clear();
  for (JsonVariantConst r : doc["reminders"].as<JsonArrayConst>()) {
    if (out.size() >= HubStore::MAX_REMINDERS) break;
    out.push_back({r["id"] | 0, str(r, "title"), str(r, "when"), static_cast<time_t>(r["dueAt"] | (int64_t)0)});
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
    list.name = str(l, "name");
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
  }
  JsonArray ls = doc["lists"].to<JsonArray>();
  for (const List& l : lists) {
    JsonObject o = ls.add<JsonObject>();
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
  JsonArray msgs = doc["messages"].to<JsonArray>();
  for (const Message& m : messages) {
    JsonObject o = msgs.add<JsonObject>();
    o["id"] = m.id;
    o["from"] = m.from;
    o["text"] = m.text;
  }
  doc["quote"] = quote;
  doc["verseRef"] = verseRef;
  doc["verseText"] = verseText;
  doc["translatorLang"] = translatorLang;
  doc["speakMode"] = speakMode;
  doc["bibleBook"] = bibleBook;
  doc["bibleChapter"] = bibleChapter;
  doc["musicVolume"] = musicVolume;
  doc["timerEndAt"] = static_cast<int64_t>(timerEndAt);
  doc["timerTotal"] = timerTotal;
  doc["timerMode"] = timerMode;
  doc["timerPausedLeft"] = timerPausedLeft;
  doc["timerRound"] = timerRound;
  doc["stopwatchStartAt"] = static_cast<int64_t>(stopwatchStartAt);
  doc["stopwatchAccumS"] = stopwatchAccumS;
  doc["settingsRev"] = settingsRev;
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
  messages.clear();
  for (JsonVariantConst m : doc["messages"].as<JsonArrayConst>()) {
    if (messages.size() >= MAX_MESSAGES) break;
    messages.push_back({m["id"] | 0, str(m, "from"), str(m, "text")});
  }
  quote = str(doc, "quote");
  verseRef = str(doc, "verseRef");
  verseText = str(doc, "verseText");
  translatorLang = str(doc, "translatorLang");
  speakMode = doc["speakMode"] | 1;
  bibleBook = doc["bibleBook"] | 0;
  bibleChapter = doc["bibleChapter"] | 0;
  musicVolume = doc["musicVolume"] | 70;
  timerEndAt = static_cast<time_t>(doc["timerEndAt"] | (int64_t)0);
  timerTotal = doc["timerTotal"] | 0;
  timerMode = doc["timerMode"] | 0;
  timerPausedLeft = doc["timerPausedLeft"] | 0;
  timerRound = doc["timerRound"] | 1;
  stopwatchStartAt = static_cast<time_t>(doc["stopwatchStartAt"] | (int64_t)0);
  stopwatchAccumS = doc["stopwatchAccumS"] | 0;
  settingsRev = doc["settingsRev"] | 0;
  uiLang = str(doc, "uiLang");
  ttsVoice = str(doc, "ttsVoice");
  return true;
}

// Server shape (see paper/src/hub.ts):
//   { ok, now, weather: {line, detail}, reminders: [{title, when}], events: [{when, title}],
//     messages: [{from, text}], quote }
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
  messages.clear();
  for (JsonVariantConst m : doc["messages"].as<JsonArrayConst>()) {
    if (messages.size() >= MAX_MESSAGES) break;
    messages.push_back({m["id"] | 0, str(m, "from"), str(m, "text")});
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

// { rev, lang, speak: "none"|"short"|"all", musicVolume, translatorLang }
void HubStore::applySettings(JsonVariantConst s) {
  if (s.isNull()) return;
  const int rev = s["rev"] | 0;
  if (rev <= settingsRev) return;
  settingsRev = rev;
  const std::string speak = str(s, "speak");
  if (speak == "none") speakMode = 0;
  else if (speak == "all") speakMode = 2;
  else if (speak == "short") speakMode = 1;
  const int vol = s["musicVolume"] | -1;
  if (vol >= 0 && vol <= 100) musicVolume = vol;
  const std::string other = str(s, "translatorLang");
  if (!other.empty()) translatorLang = other;
  uiLang = str(s, "lang");
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

void HubStore::moveItem(const int id, const std::string& listName) {
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
  for (List& l : lists) {
    if (l.name == listName) {
      l.items.push_back(moved);
      return;
    }
  }
  lists.push_back({listName, {moved}});
}

void HubStore::removeMessage(const int id) {
  for (auto it = messages.begin(); it != messages.end(); ++it) {
    if (it->id == id) {
      messages.erase(it);
      return;
    }
  }
}
