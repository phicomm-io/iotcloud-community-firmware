#pragma once
#include <ArduinoJson.h>
#include <cstring>
extern "C" {
#include "protocol/services.h"
}
static bool screenConfigStartupAuto(JsonObjectConst doc,bool &enabled) {
  enabled=true;
  if(!doc.containsKey("startup_auto_resume"))return true;
  if(!doc["startup_auto_resume"].is<bool>()){enabled=false;return false;}
  enabled=doc["startup_auto_resume"].as<bool>();return true;
}

// Layout text stays owned by the caller through serialization. Count the
// actual live records and copied JSON trees instead of reserving 40 KiB for
// every save, even when both reminder arrays are empty.
static size_t screenConfigCapacity(const JsonDocument &weather,
                                   const JsonDocument &settings,
                                   const reminder_schedule &schedule,
                                   const reminder_notice *notices) {
  size_t capacity = JSON_OBJECT_SIZE(11) + weather.memoryUsage() +
                    settings.memoryUsage();
  for (const auto &r : schedule.rules)
    if (r.id)
      capacity += JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(10) +
                  std::strlen(r.title) + std::strlen(r.text) + 2;
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    const auto &r = notices[i];
    if (r.id)
      capacity += JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(6) +
                  std::strlen(r.title) + std::strlen(r.text) + 2;
  }
  return capacity;
}
static bool screenConfigDocument(JsonDocument &doc, const char *layout,
                                 const JsonDocument &weather,
                                 const JsonDocument &settings,
                                 const reminder_schedule &schedule,
                                 const reminder_notice *notices,
                                 const char *policy = nullptr, const char *appearance = nullptr,
                                 bool startupAuto = true) {
  doc["schema"] = 2;
  doc["startup_auto_resume"] = startupAuto;
  doc["layout"] = layout;
  if (policy) doc["policy"] = policy;
  if (appearance) doc["appearance"] = appearance;
  doc["weather"] = weather;
  doc["settings"] = settings;
  doc["timezone"] = schedule.timezone;
  doc["wake"] = schedule.wake;
  JsonArray rules = doc.createNestedArray("rules"),
            allNotices = doc.createNestedArray("notices");
  for (const auto &r : schedule.rules)
    if (r.id) {
      JsonObject o = rules.createNestedObject();
      o["id"] = r.id;
      o["kind"] = r.kind;
      o["epoch"] = r.epoch;
      o["minute"] = r.minute;
      o["days"] = r.days;
      o["wake"] = r.wake;
      o["enabled"] = r.enabled;
      o["title"] = r.title;
      o["text"] = r.text;
      o["last_fire"] = r.last_fire;
    }
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    const auto &r = notices[i];
    if (r.id) {
      JsonObject o = allNotices.createNestedObject();
      o["id"] = r.id;
      o["at"] = r.at;
      o["wake"] = r.wake;
      o["unread"] = r.unread;
      o["title"] = r.title;
      o["text"] = r.text;
    }
  }
  return !doc.overflowed();
}
