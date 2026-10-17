#include "../common/phicomm_features.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static void statistics(void) {
  feature_sample_t s[8] = {0};
  float out[4];
  unsigned effective;
  for (unsigned i = 0; i < 5; i++) {
    s[i].valid = true;
    s[i].received_ms = 100;
    s[i].value[1] = 50;
    for (unsigned f = 2; f < 4; f++)
      s[i].value[f] = (i + 1) * 10;
  }
  assert(feature_aggregate(s, 5, 200, STAT_MEAN, out, &effective) == 5 &&
         out[2] == 30);
  s[4].value[2] = 1000;
  feature_aggregate(s, 5, 200, STAT_MEDIAN, out, &effective);
  assert(out[2] == 30);
  feature_aggregate(s, 5, 200, STAT_TRIMMED, out, &effective);
  assert(out[2] == 30);
  s[0].valid = false;
  feature_aggregate(s, 5, 200, STAT_FAILBACK, out, &effective);
  assert(out[2] == 20);
  s[4].age_ms = 14900;
  assert(!feature_sample_fresh(&s[4], 200));
  feature_aggregate(s, 3, 200, STAT_MEDIAN, out, &effective);
  assert(effective == STAT_MEAN);
  assert(feature_aggregate(s, 5, 15100, STAT_MEAN, out, &effective) == 0);
  s[1].value[2] = NAN;
  assert(!feature_sample_fresh(&s[1], 100));
}
static void controls(void) {
  feature_config_t c;
  feature_defaults(&c);
  feature_pid_t p = {0};
  bool on;
  unsigned speed;
  float v[4] = {20, 50, 100, .15};
  c.linkage.enabled = 1;
  c.linkage.on_enabled = 1;
  c.linkage.off_enabled = 1;
  feature_control(&c.linkage, &p, v, 1000, false, 60, &on, &speed);
  assert(!on);
  feature_control(&c.linkage, &p, v, 16000, false, 60, &on, &speed);
  assert(!on && speed == 0);
  v[2] = 20;
  feature_control(&c.linkage, &p, v, 17000, true, 60, &on, &speed);
  assert(on);
  feature_control(&c.linkage, &p, v, 77000, true, 60, &on, &speed);
  assert(on && speed == 20);
  c.linkage.on_enabled = c.linkage.off_enabled = 0;
  c.linkage.mode = 1;
  v[2] = 1000;
  feature_control(&c.linkage, &p, v, 80000, true, 60, &on, &speed);
  assert(speed == 100);
  c.linkage.mode = 2;
  memset(&p, 0, sizeof(p));
  for (unsigned i = 0; i < 10000; i++) {
    feature_control(&c.linkage, &p, v, 1000 + i * 5000, true, 60, &on, &speed);
    assert(speed >= 20 && speed <= 100 && isfinite(p.integral));
  }
}
static void schedules(void) {
  feature_config_t c;
  feature_defaults(&c);
  c.sleep_count = 1;
  c.sleeps[0] = (feature_sleep_t){1, 1, 1, 30, 0, 1, 22 * 60, 7 * 60};
  assert(feature_sleep_at(&c, 0, 23 * 60).fan_sleep);
  assert(feature_sleep_at(&c, 1, 6 * 60).light_sleep);
  assert(!feature_sleep_at(&c, 1, 8 * 60).fan_sleep);
  c.timezone_minutes = 0;
  c.event_count = 2;
  c.events[0] = (feature_event_t){127, 1, 60, 0, 8 * 60};
  c.events[1] = (feature_event_t){127, 0, 0, 0, 22 * 60};
  uint64_t stamp;
  assert(feature_latest_event(&c, 1700000000, 0, &stamp) >= 0);
  assert(stamp <= 1700000000);
  assert(feature_latest_event(&c, stamp, stamp, &stamp) == -1);
}
static void lights(void) {
  feature_config_t c;
  feature_defaults(&c);
  float v[4] = {25, 60, 50, .1};
  bool valid[4] = {true, true, true, true};
  uint32_t pixels[32];
  for (unsigned n = 1; n <= 4; n++) {
    c.light.segments = n;
    for (unsigned i = 0; i < 4; i++)
      c.light.segment[i].display = DISPLAY_SUITABILITY;
    feature_render(&c.light, v, valid, 60, 255, true, 0, pixels);
    unsigned off = 0;
    for (unsigned i = 0; i < 32; i++)
      off += pixels[i] == 0;
    assert(off == (n == 1 ? 0 : n));
  }
  c.light.segments = 4;
  valid[0] = false;
  feature_render(&c.light, v, valid, 60, 255, true, 0, pixels);
  for (unsigned i = 0; i < 8; i++)
    assert(pixels[i] == 0);
  assert(pixels[8]);
  feature_render(&c.light, v, valid, 60, 0, true, 0, pixels);
  for (unsigned i = 0; i < 32; i++)
    assert(!pixels[i]);
}
int main(void) {
  char id[13], root[128];
  assert(feature_mac("AA:BB:CC:DD:EE:01", id));
  assert(!strcmp(id, "aabbccddee01"));
  assert(!feature_mac("aa-bb-cc-dd-ee-01", id));
  assert(!feature_group_valid("bad/#"));
  feature_root("aabbccddee01", "room", "112233445566", 1, root, sizeof(root));
  assert(!strcmp(root, "room:11:22:33:44:55:66"));
  statistics();
  controls();
  schedules();
  lights();
  puts("engine scenarios passed");
  return 0;
}
