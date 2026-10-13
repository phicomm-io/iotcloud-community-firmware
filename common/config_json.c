/* Shared typed configuration codec. Reject an entire transaction on invalid
 * input. */
#include "phicomm_features.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  const char *key;
  size_t offset;
  unsigned type;
  double low, high;
} field_t;
#define U(T, k, lo, hi) {#k, offsetof(T, k), 1, lo, hi}
#define W(T, k, lo, hi) {#k, offsetof(T, k), 2, lo, hi}
#define F(T, k, lo, hi) {#k, offsetof(T, k), 3, lo, hi}
static const field_t linkage_fields[] = {
    U(feature_linkage_t, enabled, 0, 1),
    U(feature_linkage_t, mode, 0, 2),
    U(feature_linkage_t, metrics, 1, 3),
    U(feature_linkage_t, on_metrics, 1, 3),
    U(feature_linkage_t, off_metrics, 1, 3),
    U(feature_linkage_t, statistic, 0, 5),
    U(feature_linkage_t, on_enabled, 0, 1),
    U(feature_linkage_t, off_enabled, 0, 1),
    U(feature_linkage_t, on_all, 0, 1),
    U(feature_linkage_t, off_all, 0, 1),
    U(feature_linkage_t, fixed_speed, 1, 100),
    U(feature_linkage_t, min_speed, 1, 100),
    U(feature_linkage_t, max_speed, 1, 100),
    W(feature_linkage_t, debounce_seconds, 0, 3600),
    W(feature_linkage_t, min_switch_seconds, 0, 3600),
    F(feature_linkage_t, on_pm, 0, 5000),
    F(feature_linkage_t, off_pm, 0, 5000),
    F(feature_linkage_t, on_hcho, 0, 10),
    F(feature_linkage_t, off_hcho, 0, 10),
    F(feature_linkage_t, target_pm, 0.01, 5000),
    F(feature_linkage_t, target_hcho, 0.001, 10),
    F(feature_linkage_t, low_pm, 0, 5000),
    F(feature_linkage_t, high_pm, 0, 5000),
    F(feature_linkage_t, low_hcho, 0, 10),
    F(feature_linkage_t, high_hcho, 0, 10),
    F(feature_linkage_t, kp, 0, 1000),
    F(feature_linkage_t, ki, 0, 100),
    F(feature_linkage_t, kd, 0, 1000)};
static const field_t light_fields[] = {
    U(feature_light_t, effect, 0, 4), U(feature_light_t, segments, 1, 4),
    U(feature_light_t, follow_fan, 0, 1),
    F(feature_light_t, quality_pm, .01, 5000),
    F(feature_light_t, quality_hcho, .001, 10)};
static const field_t segment_fields[] = {
    U(feature_segment_t, source, 0, 5),
    U(feature_segment_t, display, 0, 2),
    F(feature_segment_t, minimum, -100, 10000),
    F(feature_segment_t, maximum, -100, 10000),
    F(feature_segment_t, suitable_low, -100, 10000),
    F(feature_segment_t, suitable_high, -100, 10000)};
static const field_t event_fields[] = {
    U(feature_event_t, days, 1, 127), U(feature_event_t, on, 0, 1),
    U(feature_event_t, speed, 1, 100), W(feature_event_t, minute, 0, 1439)};
static const field_t sleep_fields[] = {U(feature_sleep_t, days, 1, 127),
                                       U(feature_sleep_t, fan, 0, 1),
                                       U(feature_sleep_t, light, 0, 1),
                                       U(feature_sleep_t, max_speed, 1, 100),
                                       U(feature_sleep_t, brightness, 0, 255),
                                       U(feature_sleep_t, block_start, 0, 1),
                                       W(feature_sleep_t, start, 0, 1439),
                                       W(feature_sleep_t, end, 0, 1439)};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static json_object *encode_fields(const void *data, const field_t *f,
                                  unsigned n) {
  json_object *o = json_object_new_object();
  if (!o)
    return NULL;
  for (unsigned i = 0; i < n; i++) {
    const unsigned char *p = (const unsigned char *)data + f[i].offset;
    json_object_object_add(
        o, f[i].key,
        f[i].type == 3
            ? json_object_new_double(*(const float *)p)
            : json_object_new_int(f[i].type == 2 ? *(const uint16_t *)p : *p));
  }
  return o;
}
static bool decode_fields(json_object *o, void *data, const field_t *f,
                          unsigned n) {
  if (!o || !json_object_is_type(o, json_type_object))
    return false;
  for (unsigned i = 0; i < n; i++) {
    json_object *v = json_object_object_get(o, f[i].key);
    if (!v)
      continue;
    if (!json_object_is_type(v, json_type_int) &&
        !json_object_is_type(v, json_type_double) &&
        !json_object_is_type(v, json_type_boolean))
      return false;
    if (json_object_is_type(v, json_type_boolean) &&
        !(f[i].type == 1 && f[i].low == 0 && f[i].high == 1))
      return false;
    double x = json_object_get_double(v);
    if (!isfinite(x) || x < f[i].low || x > f[i].high ||
        (f[i].type != 3 && floor(x) != x))
      return false;
    unsigned char *p = (unsigned char *)data + f[i].offset;
    if (f[i].type == 3)
      *(float *)p = x;
    else if (f[i].type == 2)
      *(uint16_t *)p = x;
    else
      *p = x;
  }
  return true;
}
static bool string_field(json_object *o, const char *key, char *dst, size_t n) {
  json_object *v = json_object_object_get(o, key);
  if (!v)
    return true;
  if (!json_object_is_type(v, json_type_string))
    return false;
  const char *s = json_object_get_string(v);
  if (strlen(s) >= n || strlen(s) != (size_t)json_object_get_string_len(v) ||
      strpbrk(s, "\r\n"))
    return false;
  strcpy(dst, s);
  return true;
}
static void js(json_object *o, const char *k, const char *v) {
  json_object_object_add(o, k, json_object_new_string(v));
}
static json_object *config_encode(const feature_config_t *c, const char *d) {
  json_object *o = NULL, *a;
  if (!strcmp(d, "linkage"))
    o = encode_fields(&c->linkage, linkage_fields, COUNT(linkage_fields));
  else if (!strcmp(d, "light")) {
    o = encode_fields(&c->light, light_fields, COUNT(light_fields));
    a = json_object_new_array();
    for (unsigned i = 0; i < 4; i++) {
      json_object *s = encode_fields(&c->light.segment[i], segment_fields,
                                     COUNT(segment_fields)),
                  *colors = json_object_new_array();
      for (unsigned j = 0; j < 3; j++) {
        json_object *rgb = json_object_new_array();
        for (unsigned k = 0; k < 3; k++)
          json_object_array_add(
              rgb, json_object_new_int(c->light.segment[i].colors[j][k]));
        json_object_array_add(colors, rgb);
      }
      json_object_object_add(s, "colors", colors);
      json_object_array_add(a, s);
    }
    json_object_object_add(o, "segment", a);
  } else if (!strcmp(d, "schedule") || !strcmp(d, "sleep")) {
    bool sleep = !strcmp(d, "sleep");
    o = json_object_new_object();
    a = json_object_new_array();
    for (unsigned i = 0; i < (sleep ? c->sleep_count : c->event_count); i++)
      json_object_array_add(a, sleep
                                   ? encode_fields(&c->sleeps[i], sleep_fields,
                                                   COUNT(sleep_fields))
                                   : encode_fields(&c->events[i], event_fields,
                                                   COUNT(event_fields)));
    json_object_object_add(o, sleep ? "windows" : "events", a);
  } else if (!strcmp(d, "time")) {
    o = json_object_new_object();
    js(o, "ntp_host", c->ntp_host);
    json_object_object_add(o, "timezone_minutes",
                           json_object_new_int(c->timezone_minutes));
  } else if (!strcmp(d, "binding")) {
    o = json_object_new_object();
    js(o, "group", c->group);
    json_object_object_add(o, "group_force_mqtt",
                           json_object_new_boolean(c->group_force_mqtt));
    json_object_object_add(o, "group_recovery",
                           json_object_new_boolean(c->group_recovery));
    json_object_object_add(o, "group_timeout_ms",
                           json_object_new_int(c->group_timeout_ms));
    json_object_object_add(o, "group_recovery_seconds",
                           json_object_new_int(c->group_recovery_seconds));

    a = json_object_new_array();
    for (unsigned i = 0; i < c->peer_count; i++) {
      const feature_peer_t *p = &c->peers[i];
      json_object *s = json_object_new_object();
      js(s, "mac", p->id);
      js(s, "ip", p->ip);
      js(s, "group", p->group);
      json_object_object_add(s, "kind", json_object_new_int(p->kind));
      json_object_object_add(s, "confirmed",
                             json_object_new_boolean(p->confirmed));
      json_object_object_add(s, "force_mqtt",
                             json_object_new_boolean(p->force_mqtt));
      json_object_object_add(s, "recovery",
                             json_object_new_boolean(p->recovery));
      json_object_object_add(s, "timeout_ms",
                             json_object_new_int(p->timeout_ms));
      json_object_object_add(s, "recovery_seconds",
                             json_object_new_int(p->recovery_seconds));
      json_object_array_add(a, s);
    }
    json_object_object_add(o, "peers", a);
    a = json_object_new_array();
    for (unsigned i = 0; i < c->member_count; i++) {
      json_object *s = json_object_new_object();
      js(s, "mac", c->members[i].id);
      js(s, "ip", c->members[i].ip);
      json_object_array_add(a, s);
    }
    json_object_object_add(o, "members", a);
  }
  if (o)
    json_object_object_add(o, "revision", json_object_new_int(c->revision));
  return o;
}
static bool config_decode(feature_config_t *c, const char *d, json_object *o) {
  if (!json_object_is_type(o, json_type_object))
    return false;
  if (!strcmp(d, "linkage")) {
    feature_linkage_t *l = &c->linkage;
    if (!decode_fields(o, l, linkage_fields, COUNT(linkage_fields)))
      return false;
    if (json_object_object_get(o, "metrics")) {
      if (!json_object_object_get(o, "on_metrics"))
        l->on_metrics = l->metrics;
      if (!json_object_object_get(o, "off_metrics"))
        l->off_metrics = l->metrics;
    }
    return l->min_speed <= l->max_speed && l->low_pm < l->high_pm &&
           l->low_hcho < l->high_hcho;
  }
  if (!strcmp(d, "light")) {
    if (!decode_fields(o, &c->light, light_fields, COUNT(light_fields)))
      return false;
    json_object *a = json_object_object_get(o, "segment");
    if (!a && json_object_object_get(o, "segments"))
      feature_light_layout(&c->light, c->light.segments);
    if (a) {
      if (!json_object_is_type(a, json_type_array) ||
          json_object_array_length(a) > 4)
        return false;
      for (unsigned i = 0; i < (unsigned)json_object_array_length(a); i++) {
        json_object *s = json_object_array_get_idx(a, i);
        feature_segment_t *t = &c->light.segment[i];
        if (!decode_fields(s, t, segment_fields, COUNT(segment_fields)) ||
            t->minimum >= t->maximum || t->suitable_low > t->suitable_high)
          return false;
        json_object *colors = json_object_object_get(s, "colors");
        if (colors) {
          if (!json_object_is_type(colors, json_type_array) ||
              json_object_array_length(colors) != 3)
            return false;
          for (unsigned j = 0; j < 3; j++) {
            json_object *rgb = json_object_array_get_idx(colors, j);
            if (!json_object_is_type(rgb, json_type_array) ||
                json_object_array_length(rgb) != 3)
              return false;
            for (unsigned k = 0; k < 3; k++) {
              json_object *v = json_object_array_get_idx(rgb, k);
              int x = json_object_get_int(v);
              if (!json_object_is_type(v, json_type_int) || x < 0 || x > 255)
                return false;
              t->colors[j][k] = x;
            }
          }
        }
      }
    }
    return true;
  }
  if (!strcmp(d, "schedule") || !strcmp(d, "sleep")) {
    bool s = !strcmp(d, "sleep");
    json_object *a = json_object_object_get(o, s ? "windows" : "events");
    if (!a || !json_object_is_type(a, json_type_array) ||
        json_object_array_length(a) > (s ? 4 : 8))
      return false;
    unsigned n = json_object_array_length(a);
    for (unsigned i = 0; i < n; i++) {
      if (s) {
        feature_sleep_t w = {127, 1, 1, 30, 0, 1, 1320, 420};
        if (!decode_fields(json_object_array_get_idx(a, i), &w, sleep_fields,
                           COUNT(sleep_fields)) ||
            w.start == w.end)
          return false;
        c->sleeps[i] = w;
      } else {
        feature_event_t e = {127, 1, 60, 0, 480};
        if (!decode_fields(json_object_array_get_idx(a, i), &e, event_fields,
                           COUNT(event_fields)))
          return false;
        c->events[i] = e;
      }
    }
    if (s)
      c->sleep_count = n;
    else
      c->event_count = n;
    return true;
  }
  if (!strcmp(d, "time")) {
    if (!string_field(o, "ntp_host", c->ntp_host, sizeof(c->ntp_host)) ||
        !c->ntp_host[0])
      return false;
    json_object *v = json_object_object_get(o, "timezone_minutes");
    if (v) {
      int x = json_object_get_int(v);
      if (!json_object_is_type(v, json_type_int) || x < -720 || x > 840)
        return false;
      c->timezone_minutes = x;
    }
    return true;
  }
  if (!strcmp(d, "binding")) {
    const field_t group_fields[] = {
        U(feature_config_t, group_force_mqtt, 0, 1),
        U(feature_config_t, group_recovery, 0, 1),
        W(feature_config_t, group_timeout_ms, 100, 10000),
        W(feature_config_t, group_recovery_seconds, 5, 3600)};
    if (!decode_fields(o, c, group_fields, COUNT(group_fields)))
      return false;

    if (!string_field(o, "group", c->group, sizeof(c->group)) ||
        (c->group[0] && !feature_group_valid(c->group)))
      return false;
    json_object *a = json_object_object_get(o, "peers");
    if (a) {
      if (!json_object_is_type(a, json_type_array) ||
          json_object_array_length(a) > 8)
        return false;
      feature_peer_t old[8];
      memcpy(old, c->peers, sizeof(old));
      unsigned oldn = c->peer_count;
      c->peer_count = json_object_array_length(a);
      for (unsigned i = 0; i < c->peer_count; i++) {
        json_object *v = json_object_array_get_idx(a, i);
        feature_peer_t *p = &c->peers[i];
        memset(p, 0, sizeof(*p));
        p->timeout_ms = 1000;
        p->recovery_seconds = 60;
        p->recovery = 1;
        p->kind = FEATURE_M1;
#ifndef FEATURE_DEVICE_A1
        p->kind = FEATURE_A1;
#endif
        char mac[18] = {0};
        if (!string_field(v, "mac", mac, sizeof(mac)) ||
            !feature_mac(mac, p->id))
          return false;
        for (unsigned j = 0; j < i; j++)
          if (!strcmp(p->id, c->peers[j].id))
            return false;
        for (unsigned j = 0; j < oldn; j++)
          if (!strcmp(p->id, old[j].id)) {
            *p = old[j];
            break;
          }
        if (!string_field(v, "ip", p->ip, sizeof(p->ip)) ||
            !string_field(v, "group", p->group, sizeof(p->group)))
          return false;
        const field_t pf[] = {U(feature_peer_t, kind, 1, 3),
                              U(feature_peer_t, force_mqtt, 0, 1),
                              U(feature_peer_t, recovery, 0, 1),
                              W(feature_peer_t, timeout_ms, 100, 10000),
                              W(feature_peer_t, recovery_seconds, 5, 3600)};
        if (!decode_fields(v, p, pf, COUNT(pf)) ||
            (p->group[0] && !feature_group_valid(p->group)))
          return false;
        if (p->ip[0] &&
            (inet_addr(p->ip) == INADDR_NONE || inet_addr(p->ip) == 0))
          return false;
      }
    }
    a = json_object_object_get(o, "members");
    if (a) {
      if (!json_object_is_type(a, json_type_array) ||
          json_object_array_length(a) > 8)
        return false;
      c->member_count = json_object_array_length(a);
      for (unsigned i = 0; i < c->member_count; i++) {
        json_object *v = json_object_array_get_idx(a, i);
        char mac[18] = {0};
        if (!string_field(v, "mac", mac, sizeof(mac)) ||
            !feature_mac(mac, c->members[i].id) ||
            !string_field(v, "ip", c->members[i].ip, 16))
          return false;
        for (unsigned j = 0; j < i; j++)
          if (!strcmp(c->members[i].id, c->members[j].id))
            return false;
      }
    }
    return true;
  }
  return false;
}
