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
  JsonArray ev = doc["events"].to<JsonArray>();
  for (const Event& e : events) {
    JsonObject o = ev.add<JsonObject>();
    o["when"] = e.when;
    o["title"] = e.title;
  }
  JsonArray msgs = doc["messages"].to<JsonArray>();
  for (const Message& m : messages) {
    JsonObject o = msgs.add<JsonObject>();
    o["from"] = m.from;
    o["text"] = m.text;
  }
  doc["quote"] = quote;
}

bool HubStore::fromJson(JsonVariantConst doc) {
  syncedAt = static_cast<time_t>(doc["syncedAt"] | (int64_t)0);
  lastAttemptAt = static_cast<time_t>(doc["lastAttemptAt"] | (int64_t)0);
  weatherLine = str(doc, "weatherLine");
  weatherDetail = str(doc, "weatherDetail");
  reminderTitle = str(doc, "reminderTitle");
  reminderWhen = str(doc, "reminderWhen");
  parseReminders(doc, reminders);
  parseLists(doc, lists);
  events.clear();
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>()) {
    if (events.size() >= MAX_EVENTS) break;
    events.push_back({str(e, "when"), str(e, "title")});
  }
  messages.clear();
  for (JsonVariantConst m : doc["messages"].as<JsonArrayConst>()) {
    if (messages.size() >= MAX_MESSAGES) break;
    messages.push_back({str(m, "from"), str(m, "text")});
  }
  quote = str(doc, "quote");
  return true;
}

// Server shape (see paper/src/hub.ts):
//   { ok, now, weather: {line, detail}, reminders: [{title, when}], events: [{when, title}],
//     messages: [{from, text}], quote }
void HubStore::applyServer(JsonVariantConst doc) {
  weatherLine = str(doc["weather"], "line");
  weatherDetail = str(doc["weather"], "detail");
  parseReminders(doc, reminders);
  parseLists(doc, lists);
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
    messages.push_back({str(m, "from"), str(m, "text")});
  }
  quote = str(doc, "quote");
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
