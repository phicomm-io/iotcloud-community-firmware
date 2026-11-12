#pragma once
#include "scene.h"
#define REMINDER_MAX 32
enum { REM_ONCE, REM_DAILY, REM_WEEKLY };
enum { REM_RESPECT_DND, REM_ALWAYS_WAKE, REM_UNREAD_ONLY, REM_INHERIT = 255 };
typedef struct {
  uint32_t id, epoch, last_fire;
  uint16_t minute;
  uint8_t kind, days, wake;
  bool enabled;
  char title[65], text[129];
} reminder_rule;
typedef struct {
  uint32_t id, at;
  uint8_t wake;
  bool unread;
  char title[65], text[129];
} reminder_notice;
typedef struct {
  uint32_t cursor;
  bool started;
  int16_t timezone;
  uint8_t wake;
  reminder_rule rules[REMINDER_MAX];
} reminder_schedule;
bool reminder_validate(const reminder_rule *);
/* No backlog on boot or >90 second clock jumps; last_fire persists on ESP. */
unsigned reminder_due(reminder_schedule *, uint32_t now, bool time_valid,
                      uint32_t ids[REMINDER_MAX]);
bool reminder_should_wake(uint8_t policy, bool dnd, bool ota);
typedef struct {
  char date[11], text[49];
  float low, high;
  uint8_t valid;
} weather_day;
typedef struct {
  bool valid, expired;
  uint32_t at, ttl, observed;
  uint8_t fields;
  float temperature, feels, humidity, wind;
  char provider[17], location[49], text[49];
  weather_day days[3];
} screen_weather;
bool weather_receive(screen_weather *, const uint8_t *, size_t, uint32_t);
bool reminder_receive(reminder_notice[REMINDER_MAX], const uint8_t *, size_t);
