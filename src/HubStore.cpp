#include "HubStore.h"

namespace {
std::string str(JsonVariantConst v, const char* key) {
  const char* s = v[key] | "";
  return std::string(s);
}
}  // namespace

void HubStore::toJson(JsonDocument& doc) const {
  doc["syncedAt"] = static_cast<int64_t>(syncedAt);
  doc["lastAttemptAt"] = static_cast<int64_t>(lastAttemptAt);
  doc["weatherLine"] = weatherLine;
  doc["weatherDetail"] = weatherDetail;
  doc["reminderTitle"] = reminderTitle;
  doc["reminderWhen"] = reminderWhen;
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
  reminderTitle.clear();
  reminderWhen.clear();
  JsonArrayConst reminders = doc["reminders"].as<JsonArrayConst>();
  if (reminders.size() > 0) {
    reminderTitle = str(reminders[0], "title");
    reminderWhen = str(reminders[0], "when");
  }
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
