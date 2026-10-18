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
static void idle_control(void) {
  feature_config_t c;feature_defaults(&c);
  c.linkage.enabled=1;c.linkage.min_speed=0;c.linkage.max_speed=70;
  c.linkage.debounce_seconds=5;c.linkage.min_switch_seconds=10;
  feature_pid_t p={0};float v[4]={22,45,0,.01};bool on;unsigned speed;
  feature_control(&c.linkage,&p,v,1000,true,40,&on,&speed);assert(on && speed==40);
  feature_control(&c.linkage,&p,v,6000,true,40,&on,&speed);assert(on && speed==0);
  v[2]=1000;
  feature_control(&c.linkage,&p,v,7000,true,0,&on,&speed);assert(on && speed==0);
  feature_control(&c.linkage,&p,v,12000,true,0,&on,&speed);assert(on && speed==0);
  feature_control(&c.linkage,&p,v,16000,true,0,&on,&speed);assert(on && speed==70);
  feature_control(&c.linkage,&p,v,30000,false,70,&on,&speed);assert(!on && speed==0);
  c.linkage.mode=2; c.linkage.kp=20;c.linkage.ki=.1f;
  memset(&p,0,sizeof(p));
  feature_control(&c.linkage,&p,v,1000,true,0,&on,&speed);assert(isfinite(p.integral) && fabsf(p.integral)<1000);
}
static void hysteresis(void) {
  feature_config_t c; feature_defaults(&c);
  c.linkage.enabled=1; c.linkage.hysteresis_enabled=1;
  c.linkage.on_pm=50; c.linkage.target_pm=35;
  c.linkage.min_speed=20; c.linkage.max_speed=70;
  c.linkage.fixed_speed=57; c.linkage.debounce_seconds=5;
  c.linkage.min_switch_seconds=10;
  float v[4]={22,45,40,.09f}; bool on; unsigned speed;
  for (unsigned mode=0; mode<3; mode++) {
    c.linkage.mode=mode; feature_pid_t p={0};
    feature_control(&c.linkage,&p,v,1000,true,60,&on,&speed);
    assert(on && speed==0); /* Boot inside band: wait for start threshold. */
    v[2]=50;
    feature_control(&c.linkage,&p,v,2000,true,0,&on,&speed); assert(speed==0);
    v[2]=49;
    feature_control(&c.linkage,&p,v,6000,true,0,&on,&speed); assert(speed==0);
    v[2]=50;
    feature_control(&c.linkage,&p,v,7000,true,0,&on,&speed); assert(speed==0);
    feature_control(&c.linkage,&p,v,12000,true,0,&on,&speed);
    assert(on && speed>=20 && speed<=70 && p.gate_running);
    unsigned running=speed;
    v[2]=40;
    feature_control(&c.linkage,&p,v,13000,true,running,&on,&speed);
    assert(speed>=20 && p.gate_running);
    v[2]=35;
    feature_control(&c.linkage,&p,v,14000,true,speed,&on,&speed);
    feature_control(&c.linkage,&p,v,19000,true,speed,&on,&speed);
    assert(speed>=20); /* Minimum run time still active. */
    feature_control(&c.linkage,&p,v,22000,true,speed,&on,&speed);
    assert(on && speed==0 && !p.gate_running && !p.initialized);
    v[2]=50;
    feature_control(&c.linkage,&p,v,23000,true,0,&on,&speed);
    feature_control(&c.linkage,&p,v,28000,true,0,&on,&speed); assert(speed==0);
    feature_control(&c.linkage,&p,v,32000,true,0,&on,&speed); assert(speed>=20);
    feature_control(&c.linkage,&p,v,33000,false,speed,&on,&speed);
    assert(!on && speed==0 && !p.gate_running);
    v[2]=40;
  }
  feature_pid_t p={0}; c.linkage.metrics=3; c.linkage.mode=0;
  c.linkage.debounce_seconds=c.linkage.min_switch_seconds=0;
  c.linkage.on_hcho=.1f; c.linkage.target_hcho=.08f;
  v[2]=40; v[3]=.1f;
  feature_control(&c.linkage,&p,v,1000,true,0,&on,&speed); assert(speed==57);
  v[2]=35; v[3]=.09f;
  feature_control(&c.linkage,&p,v,2000,true,speed,&on,&speed); assert(speed==57);
  v[3]=.08f;
  feature_control(&c.linkage,&p,v,3000,true,speed,&on,&speed); assert(on && speed==0);
  /* A zero inner demand cannot bypass the outer running latch. */
  c.linkage.mode=1; c.linkage.min_speed=0; c.linkage.low_pm=200;
  c.linkage.high_pm=300; v[2]=50;
  feature_control(&c.linkage,&p,v,4000,true,0,&on,&speed); assert(speed==1);
  /* Unsigned timers remain valid across the millisecond counter rollover. */
  memset(&p,0,sizeof(p)); c.linkage.debounce_seconds=5;
  feature_control(&c.linkage,&p,v,UINT32_MAX-2000,true,0,&on,&speed); assert(speed==0);
  feature_control(&c.linkage,&p,v,3000,true,0,&on,&speed); assert(speed==1);
  puts("hysteresis: all algorithms, boundaries, cancellation, dwell, multi-metric, power and rollover passed");
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
static void co2_linkage(void) {
  feature_sample_t s[8]={0}; float ppm=0;
  for(unsigned i=0;i<3;i++) {s[i].co2_supported=true;s[i].co2_valid=true;s[i].co2=800+400*i;s[i].co2_received_ms=100;}
  assert(feature_co2_aggregate(s,4,200,STAT_MEAN,&ppm)==3 && ppm==1200);
  s[1].co2_age_ms=19900;
  assert(!feature_co2_fresh(&s[1],200));
  assert(feature_co2_aggregate(s,4,200,STAT_MEAN,&ppm)==2 && ppm==1200);
  s[0].co2=NAN;s[2].co2=0;
  assert(feature_co2_aggregate(s,4,200,STAT_MEAN,&ppm)==0);
  s[0].co2=900;s[0].co2_received_ms=UINT32_MAX-100;s[0].co2_age_ms=0;
  assert(feature_co2_fresh(&s[0],100));
  assert(!feature_co2_fresh(&s[0],20000));
  feature_config_t c;feature_defaults(&c);
  c.linkage.enabled=1;c.linkage.metrics=4;c.linkage.min_speed=0;c.linkage.max_speed=80;
  c.linkage.debounce_seconds=0;c.linkage.min_switch_seconds=0;
  float v[4]={20,40,1000,1};bool on;unsigned speed;feature_pid_t p={0};
  c.linkage.mode=1;
  feature_control_co2(&c.linkage,&c.co2,&p,v,800,1000,true,0,&on,&speed);assert(on && speed==0);
  feature_control_co2(&c.linkage,&c.co2,&p,v,2000,6000,true,0,&on,&speed);assert(on && speed==80);
  for(unsigned mask=1;mask<=7;mask++) {
    c.linkage.metrics=mask;c.linkage.mode=0;c.linkage.fixed_speed=55;c.linkage.hysteresis_enabled=1;
    memset(&p,0,sizeof(p));v[2]=0;v[3]=0;
    feature_control_co2(&c.linkage,&c.co2,&p,v,500,1000,true,0,&on,&speed);assert(speed==0);
    if(mask&1)v[2]=c.linkage.on_pm;
    else if(mask&2)v[3]=c.linkage.on_hcho;
    feature_control_co2(&c.linkage,&c.co2,&p,v,(mask&3)?500:2000,2000,true,0,&on,&speed);assert(speed==55);
    v[2]=v[3]=0;
    feature_control_co2(&c.linkage,&c.co2,&p,v,500,3000,true,55,&on,&speed);assert(speed==0);
  }
  c.linkage.metrics=4;c.linkage.hysteresis_enabled=0;c.linkage.mode=2;memset(&p,0,sizeof(p));
  for(unsigned i=0;i<100;i++) {
    feature_control_co2(&c.linkage,&c.co2,&p,v,2500,1000+i*5000,true,0,&on,&speed);
    assert(on && speed<=80 && isfinite(p.integral));
  }
  feature_control_co2(&c.linkage,&c.co2,&p,v,2500,600000,false,80,&on,&speed);assert(!on && speed==0);
  puts("Optional CO2 aggregation, independent freshness, rollover and seven control metric combinations passed");
}
int main(void) {
  char id[13], root[128];
  assert(feature_mac("AA:BB:CC:DD:EE:01", id));
  assert(!strcmp(id, "aabbccddee01"));
  assert(!feature_mac("aa-bb-cc-dd-ee-01", id));
  assert(!feature_group_valid("bad/#"));
  feature_root("aabbccddee01", "room", "112233445566", 1, root, sizeof(root));
  assert(!strcmp(root, "room:11:22:33:44:55:66"));
  co2_linkage();
  statistics();
  controls();
  idle_control();
  hysteresis();
  schedules();
  lights();
  puts("engine scenarios passed");
  return 0;
}
