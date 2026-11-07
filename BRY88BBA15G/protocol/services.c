#include "services.h"
#include <math.h>
#include <string.h>
bool reminder_validate(const reminder_rule *r) {
  return r && r->id && r->kind <= REM_WEEKLY && r->minute < 1440 &&
         (r->wake <= REM_UNREAD_ONLY || r->wake == REM_INHERIT) &&
         (r->kind != REM_WEEKLY || (r->days && !(r->days & 128))) &&
         (r->kind != REM_ONCE || r->epoch >= 1700000000u) &&
         scene_utf8(r->title, sizeof(r->title)) &&
         scene_utf8(r->text, sizeof(r->text));
}
unsigned reminder_due(reminder_schedule *s, uint32_t now, bool valid,
                      uint32_t ids[REMINDER_MAX]) {
  if (!valid || now < 1700000000u) {
    s->started = false;
    return 0;
  }
  if (!s->started) {
    s->started = true;
    s->cursor = now;
    return 0;
  }
  uint32_t delta = now - s->cursor;
  if (!delta)
    return 0;
  if (delta > 90) {
    s->cursor = now;
    return 0;
  }
  uint32_t before = s->cursor;
  s->cursor = now;
  unsigned count = 0;
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    reminder_rule *r = &s->rules[i];
    if (!r->enabled || !reminder_validate(r))
      continue;
    uint32_t due = r->epoch;
    if (r->kind != REM_ONCE) {
      int64_t local = (int64_t)now + s->timezone * 60;
      int64_t day = local / 86400;
      unsigned weekday = (unsigned)((day + 3) % 7);
      if (r->kind == REM_WEEKLY && !(r->days & (1u << weekday)))
        continue;
      int64_t epoch = day * 86400 + r->minute * 60 - s->timezone * 60;
      if (epoch < 0 || epoch > UINT32_MAX)
        continue;
      due = (uint32_t)epoch;
    }
    if (due > before && due <= now && due > r->last_fire) {
      r->last_fire = due;
      ids[count++] = r->id;
    }
  }
  return count;
}
bool reminder_should_wake(uint8_t policy, bool dnd, bool ota) {
  return !ota &&
         (policy == REM_ALWAYS_WAKE || (policy == REM_RESPECT_DND && !dnd));
}
static bool getfloat(const uint8_t *p, float *out) {
  uint32_t u = scene_u32(p);
  memcpy(out, &u, 4);
  return isfinite(*out) && fabsf(*out) < 1e6f;
}
bool weather_receive(screen_weather *w, const uint8_t *p, size_t n,
                     uint32_t now) {
  /* Fixed v1 normalized weather, no API keys or provider-specific JSON. */
  if (n != 353 || p[0] != SC_MSG_WEATHER || p[1] > 15)
    return false;
  screen_weather v = {0};
  v.fields = p[1];
  v.observed = scene_u32(p + 2);
  v.ttl = scene_u32(p + 6);
  if (!v.ttl || v.ttl > 86400)
    return false;
  if (!getfloat(p + 10, &v.temperature) || !getfloat(p + 14, &v.feels) ||
      !getfloat(p + 18, &v.humidity) || !getfloat(p + 22, &v.wind) ||
      (v.fields & 4 && (v.humidity < 0 || v.humidity > 100)))
    return false;
  memcpy(v.provider, p + 26, 17);
  memcpy(v.location, p + 43, 49);
  memcpy(v.text, p + 92, 49);
  if (!scene_utf8(v.provider, 17) || !scene_utf8(v.location, 49) ||
      !scene_utf8(v.text, 49))
    return false;
  for (unsigned i = 0; i < 3; i++) {
    const uint8_t *q = p + 141 + 70 * i;
    weather_day *d = &v.days[i];
    d->valid = q[0];
    if (d->valid > 1)
      return false;
    memcpy(d->date, q + 1, 11);
    memcpy(d->text, q + 12, 49);
    if (!scene_utf8(d->date, 11) || !scene_utf8(d->text, 49) ||
        !getfloat(q + 61, &d->low) || !getfloat(q + 65, &d->high))
      return false;
  }
  v.ttl *= 1000;
  v.at = now;
  v.valid = true;
  *w = v;
  return true;
}
bool reminder_receive(reminder_notice items[REMINDER_MAX], const uint8_t *p,
                      size_t n) {
  if (n != 204 || p[0] != SC_MSG_REMINDER || p[9] > REM_UNREAD_ONLY ||
      p[10] > 1)
    return false;
  reminder_notice v = {0};
  v.id = scene_u32(p + 1);
  v.at = scene_u32(p + 5);
  v.wake = p[9];
  v.unread = p[10];
  memcpy(v.title, p + 11, 65);
  memcpy(v.text, p + 76, 128);
  v.text[128] = 0;
  if (!v.id || !scene_utf8(v.title, 65) || !scene_utf8(v.text, 129))
    return false;
  unsigned slot = REMINDER_MAX;
  for (unsigned i = 0; i < REMINDER_MAX; i++) {
    if (items[i].id == v.id) {
      slot = i;
      break;
    }
    if (slot == REMINDER_MAX && !items[i].unread)
      slot = i;
  }
  if (slot == REMINDER_MAX)
    return false;
  items[slot] = v;
  return true;
}
