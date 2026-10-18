#include "phicomm_features.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static float clip(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
void feature_co2_defaults(feature_co2_config_t *c) {
  *c = (feature_co2_config_t){1500, 1000, 800, 2000};
}
void feature_defaults(feature_config_t *c) {
  memset(c, 0, sizeof(*c));
  c->magic = FEATURE_MAGIC;
  c->revision = 1;
  feature_co2_defaults(&c->co2);
  strcpy(c->ntp_host, "pool.ntp.org");
  c->timezone_minutes = 480;
  c->linkage.metrics = c->linkage.on_metrics = c->linkage.off_metrics = 1;
  c->scheduled_speed = 60;
  c->linkage.fixed_speed = 60;
  c->linkage.mode = 1;
  c->automatic.fallback_speed = 30;
  for (unsigned i=0;i<4;i++) c->automatic.night[i].fallback_speed = 30;
  c->linkage.min_speed = 20;
  c->linkage.max_speed = 100;
  c->linkage.debounce_seconds = 15;
  c->linkage.min_switch_seconds = 60;
  c->linkage.on_pm = 50;
  c->linkage.off_pm = 35;
  c->linkage.target_pm = 35;
  c->linkage.on_hcho = .100f;
  c->linkage.off_hcho = .080f;
  c->linkage.target_hcho = .080f;
  c->linkage.low_pm = 35;
  c->linkage.high_pm = 150;
  c->linkage.low_hcho = .080f;
  c->linkage.high_hcho = .300f;
  c->linkage.kp = 20;
  c->linkage.ki = .02f;
  c->linkage.off_all = 1;
  c->group_timeout_ms = 1000;
  c->group_recovery_seconds = 60;
  c->group_recovery = 1;
  c->light.quality_pm = 35;
  c->light.quality_hcho = .08f;
  feature_light_layout(&c->light, 3);
}
void feature_light_layout(feature_light_t *l, unsigned n) {
  static const unsigned sources[4][4] = {
      {FIELD_QUALITY},
      {FIELD_PM25, FIELD_HCHO},
      {FIELD_TEMP, FIELD_HUMIDITY, FIELD_QUALITY},
      {FIELD_TEMP, FIELD_HUMIDITY, FIELD_PM25, FIELD_HCHO}};
  if (n < 1 || n > 4)
    return;
  l->segments = n;
  for (unsigned i = 0; i < 4; i++) {
    feature_segment_t *s = &l->segment[i];
    memset(s, 0, sizeof(*s));
    s->source = i < n ? sources[n - 1][i] : FIELD_QUALITY;
    s->display = DISPLAY_BOTH;
    s->colors[0][2] = 255;
    s->colors[1][1] = 255;
    s->colors[2][0] = 255;
    switch (s->source) {
    case FIELD_TEMP:
      s->minimum = -10;
      s->maximum = 50;
      s->suitable_low = 18;
      s->suitable_high = 26;
      break;
    case FIELD_HUMIDITY:
      s->maximum = 100;
      s->suitable_low = 40;
      s->suitable_high = 60;
      break;
    case FIELD_PM25:
      s->maximum = 150;
      s->suitable_high = 35;
      break;
    case FIELD_HCHO:
      s->maximum = .3f;
      s->suitable_high = .08f;
      break;
    default:
      s->maximum = 3;
      s->suitable_high = 1;
      break;
    }
  }
}

bool feature_mac(const char *s, char out[13]) {
  unsigned n = 0;
  if (!s || (strlen(s) != 12 && strlen(s) != 17))
    return false;
  for (unsigned i = 0; s[i]; ++i) {
    if (strlen(s) == 17 && i % 3 == 2) {
      if (s[i] != ':')
        return false;
      continue;
    }
    if (!isxdigit((unsigned char)s[i]) || n >= 12)
      return false;
    out[n++] = (char)tolower((unsigned char)s[i]);
  }
  out[n] = 0;
  return n == 12 && strcmp(out, "000000000000") && strcmp(out, "ffffffffffff");
}
void feature_colon_mac(const char *id, char out[18]) {
  snprintf(out, 18, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", id, id + 2, id + 4, id + 6,
           id + 8, id + 10);
}
bool feature_group_valid(const char *s) {
  if (!s || !*s || strlen(s) > 32)
    return false;
  for (; *s; ++s)
    if (((unsigned char)*s < 32 || (unsigned char)*s == 127) ||
        strchr(":/+#", *s))
      return false;
  return true;
}
void feature_root(const char *a1, const char *group, const char *m1,
                  unsigned count, char *out, size_t n) {
  char a[18], m[18];
  feature_colon_mac(a1, a);
  if (count == 1 && m1 && *m1)
    feature_colon_mac(m1, m);
  else
    strcpy(m, "00:00:00:00:00:00");
  snprintf(out, n, "%s:%s", group && *group ? group : a, m);
}
bool feature_sample_fresh(const feature_sample_t *s, uint32_t now) {
  if (!s->valid || s->age_ms >= FEATURE_STALE_MS ||
      (uint32_t)(now - s->received_ms) >= FEATURE_STALE_MS - s->age_ms)
    return false;
  for (unsigned i = 0; i < 4; ++i)
    if (!isfinite(s->value[i]))
      return false;
  return s->value[1] >= 0 && s->value[1] <= 100 && s->value[2] >= 0 &&
         s->value[3] >= 0;
}
unsigned feature_aggregate(const feature_sample_t *s, unsigned count,
                           uint32_t now, unsigned method, float out[4],
                           unsigned *effective) {
  unsigned indexes[8], n = 0;
  if (count > 8)
    count = 8;
  for (unsigned i = 0; i < count; ++i)
    if (feature_sample_fresh(&s[i], now))
      indexes[n++] = i;
  unsigned use = method;
  if ((method == STAT_MEDIAN && n <= 3) || (method == STAT_TRIMMED && n < 3))
    use = STAT_MEAN;
  if (effective)
    *effective = use;
  if (!n)
    return 0;
  for (unsigned f = 0; f < 4; ++f) {
    float v[8];
    for (unsigned i = 0; i < n; ++i)
      v[i] = s[indexes[i]].value[f];
    if (use == STAT_FAILBACK) {
      out[f] = v[0];
      continue;
    }
    for (unsigned i = 1; i < n; ++i) {
      float x = v[i];
      unsigned j = i;
      while (j && v[j - 1] > x) {
        v[j] = v[j - 1];
        --j;
      }
      v[j] = x;
    }
    if (use == STAT_MAX)
      out[f] = v[n - 1];
    else if (use == STAT_MIN)
      out[f] = v[0];
    else if (use == STAT_MEDIAN)
      out[f] = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    else {
      unsigned lo = use == STAT_TRIMMED ? 1 : 0,
               hi = use == STAT_TRIMMED ? n - 1 : n;
      float sum = 0;
      for (unsigned i = lo; i < hi; ++i)
        sum += v[i];
      out[f] = sum / (hi - lo);
    }
  }
  return n;
}
bool feature_co2_fresh(const feature_sample_t *s, uint32_t now) {
  return s->co2_supported && s->co2_valid && isfinite(s->co2) && s->co2 > 0 && s->co2 <= 65535 &&
         s->co2_age_ms < 20000u && (uint32_t)(now-s->co2_received_ms) < 20000u-s->co2_age_ms;
}
unsigned feature_co2_aggregate(const feature_sample_t *samples, unsigned count, uint32_t now, unsigned method, float *out) {
  feature_sample_t fresh[8] = {0};
  unsigned n = 0;
  for (unsigned i=0; i<count && i<8; i++) if (feature_co2_fresh(&samples[i],now)) {
    fresh[n].valid=true; fresh[n].received_ms=now; fresh[n++].value[0]=samples[i].co2;
  }
  float values[4]; unsigned result=feature_aggregate(fresh,n,now,method,values,NULL);
  if(result) *out=values[0];
  return result;
}
static bool control(const feature_linkage_t *c, const feature_co2_config_t *co2, float ppm, feature_pid_t *p,
                     const float v[4], uint32_t now, bool on, unsigned speed,
                     bool *next_on, unsigned *next_speed) {
  if (!c->enabled)
    return false;
  *next_on = on;
  *next_speed = speed;
  /* Power is owned by the user, including while the automatic motor idles. */
  if (!on) {
    *next_speed = 0;
    memset(p, 0, sizeof(*p));
    return true;
  }
  /* The gate owns motor idle independently of the speed controller. Any
   * selected pollutant can start it; all selected pollutants must meet target
   * before it stops. A cancelled condition restarts the continuous timer. */
  if (c->hysteresis_enabled) {
    bool start = ((c->metrics & 1) && v[2] >= c->on_pm) ||
                 ((c->metrics & 2) && v[3] >= c->on_hcho) ||
                 ((c->metrics & 4) && co2 && ppm >= co2->on_co2);
    bool stop = (!(c->metrics & 1) || v[2] <= c->target_pm) &&
                (!(c->metrics & 2) || v[3] <= c->target_hcho) &&
                (!(c->metrics & 4) || (co2 && ppm <= co2->target_co2));
    bool desired = p->gate_running ? !stop : start;
    int condition = desired == p->gate_running ? 0 : desired ? 1 : -1;
    if (condition != p->condition) {
      p->condition = condition;
      p->condition_since = now;
    }
    if (condition &&
        (uint32_t)(now - p->condition_since) >= c->debounce_seconds * 1000u &&
        (!p->switched || (uint32_t)(now - p->switch_ms) >= c->min_switch_seconds * 1000u)) {
      p->gate_running = desired;
      p->switch_ms = now;
      p->switched = true;
      p->condition = 0;
      p->initialized = false;
    }
    if (!p->gate_running) {
      p->initialized = false;
      *next_speed = 0;
      return true;
    }
  }
  float output = c->fixed_speed;
  if (c->mode == 1) {
    float a = (v[2] - c->low_pm) / (c->high_pm - c->low_pm),
          b = (v[3] - c->low_hcho) / (c->high_hcho - c->low_hcho);
    float r = -INFINITY;
    if(c->metrics & 1) r=fmaxf(r,a);
    if(c->metrics & 2) r=fmaxf(r,b);
    if((c->metrics & 4) && co2) r=fmaxf(r,(ppm-co2->low_co2)/(co2->high_co2-co2->low_co2));
    output = c->min_speed + clip(r, 0, 1) * (c->max_speed - c->min_speed);
  } else if (c->mode == 2) {
    float a = v[2] / c->target_pm, b = v[3] / c->target_hcho;
    float ratio = -INFINITY;
    if(c->metrics & 1) ratio=fmaxf(ratio,a);
    if(c->metrics & 2) ratio=fmaxf(ratio,b);
    if((c->metrics & 4) && co2) ratio=fmaxf(ratio,ppm/co2->target_co2);
    float e = ratio - 1;
    float dt = p->initialized ? (uint32_t)(now - p->last_ms) / 1000.f : 5.f;
    if (dt <= 0 || dt > 15) {
      dt = 5;
      p->initialized = false;
    }
    if (!p->initialized) {
      p->integral = ((float)speed - c->min_speed - c->kp * e);
      p->previous = e;
      p->derivative = 0;
    }
    float d = (e - p->previous) / dt;
    p->derivative += (dt / (10 + dt)) * (d - p->derivative);
    float candidate = p->integral + c->ki * e * dt;
    float raw = c->min_speed + c->kp * e + candidate + c->kd * p->derivative;
    if ((raw <= c->max_speed && raw >= c->min_speed) ||
        (raw > c->max_speed && e < 0) || (raw < c->min_speed && e > 0))
      p->integral = candidate;
    output = c->min_speed + c->kp * e + p->integral + c->kd * p->derivative;
    p->previous = e;
    p->last_ms = now;
    p->initialized = true;
  }
  unsigned target = (unsigned)(clip(output, c->min_speed, c->max_speed) + .5f);
  if (c->hysteresis_enabled) {
    /* While the gate is open even a zero-minimum algorithm must keep running. */
    *next_speed = target ? target : 1;
    return true;
  }
  int desired = target > 0 ? 1 : -1;
  if (desired != p->condition) { p->condition = desired; p->condition_since = now; }
  if ((target == 0) != (speed == 0) &&
      ((uint32_t)(now-p->condition_since) < c->debounce_seconds*1000u ||
       (p->switch_ms && (uint32_t)(now-p->switch_ms) < c->min_switch_seconds*1000u)))
    target = speed;
  else if ((target == 0) != (speed == 0)) p->switch_ms = now;
  *next_speed = (unsigned)clip(target, c->min_speed, c->max_speed);
  return true;
}
bool feature_control(const feature_linkage_t *c, feature_pid_t *p, const float v[4], uint32_t now, bool on, unsigned speed, bool *next_on, unsigned *next_speed) {
  return control(c,NULL,0,p,v,now,on,speed,next_on,next_speed);
}
bool feature_control_co2(const feature_linkage_t *c, const feature_co2_config_t *co2, feature_pid_t *p, const float v[4], float ppm, uint32_t now, bool on, unsigned speed, bool *next_on, unsigned *next_speed) {
  return control(c,co2,ppm,p,v,now,on,speed,next_on,next_speed);
}

feature_sleep_state_t feature_sleep_at(const feature_config_t *c, unsigned day,
                                       unsigned minute) {
  feature_sleep_state_t s = {false, false, false, 100, 255};
  for (unsigned i = 0; i < c->sleep_count && i < 4; ++i) {
    const feature_sleep_t *w = &c->sleeps[i];
    bool active =
        w->start < w->end
            ? ((w->days & (1u << day)) && minute >= w->start && minute < w->end)
            : ((w->days & (1u << day)) && minute >= w->start) ||
                  ((w->days & (1u << ((day + 6) % 7))) && minute < w->end);
    if (!active)
      continue;
    if (w->fan) {
      s.fan_sleep = true;
      if (w->max_speed < s.max_speed)
        s.max_speed = w->max_speed;
      s.block_start |= w->block_start != 0;
    }
    if (w->light) {
      s.light_sleep = true;
      if (w->brightness < s.brightness)
        s.brightness = w->brightness;
    }
  }
  return s;
}
int feature_latest_event(const feature_config_t *c, uint64_t utc,
                         uint64_t after, uint64_t *stamp) {
  int result = -1;
  *stamp = after;
  int64_t local = (int64_t)utc + c->timezone_minutes * 60,
          today = local / 86400;
  /* Monday=0; Unix epoch was Thursday. Search the current and preceding week.
   */
  for (unsigned back = 0; back < 8; ++back) {
    int64_t day = today - back;
    unsigned dow = (unsigned)((day + 3) % 7);
    for (unsigned i = 0; i < c->event_count && i < 8; ++i) {
      const feature_event_t *e = &c->events[i];
      if (!(e->days & (1u << dow)))
        continue;
      int64_t t = day * 86400 + e->minute * 60 - c->timezone_minutes * 60;
      if (t >= 0 && (uint64_t)t <= utc && (uint64_t)t > *stamp) {
        *stamp = t;
        result = i;
      }
    }
  }
  return result;
}
static uint32_t rgb(unsigned r, unsigned g, unsigned b, unsigned brightness) {
  return ((g * brightness / 255) << 16) | ((r * brightness / 255) << 8) |
         (b * brightness / 255);
}
void feature_render(const feature_light_t *c, const float v[4],
                    const bool valid[4], unsigned fan, unsigned brightness,
                    bool on, uint32_t now, uint32_t out[32]) {
  memset(out, 0, 32 * sizeof(*out));
  if (!on || !brightness)
    return;
  unsigned n = c->segments;
  if (n < 1 || n > 4)
    n = 1;
  unsigned at = 0;
  for (unsigned part = 0; part < n; ++part) {
    unsigned length = 32 / n + (part < 32 % n), lit = length - (n > 1);
    const feature_segment_t *s = &c->segment[part];
    float value = 0;
    bool available = true;
    if (s->source < 4) {
      value = v[s->source];
      available = valid[s->source];
    } else if (s->source == FIELD_FAN)
      value = fan;
    else {
      available = valid[2] || valid[3];
      value = fmaxf(valid[2] ? v[2] / c->quality_pm : 0,
                    valid[3] ? v[3] / c->quality_hcho : 0);
    }
    if (available) {
      unsigned color = value < s->suitable_low    ? 0
                       : value > s->suitable_high ? 2
                                                  : 1;
      if (s->display == DISPLAY_VALUE)
        color = 1;
      float level =
          s->display == DISPLAY_SUITABILITY
              ? 1
              : clip((value - s->minimum) / (s->maximum - s->minimum), 0, 1);
      unsigned b = (unsigned)(brightness * level + .5f);
      if (c->follow_fan)
        b = b * fan / 100;
      uint32_t pixel =
          rgb(s->colors[color][0], s->colors[color][1], s->colors[color][2], b);
      for (unsigned i = 0; i < lit; ++i)
        out[at + i] = pixel;
    }
    at += length;
  }
  (void)now;
}
