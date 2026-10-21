#include "../common/watchdog.h"
#include "../common/phicomm_features.h"
#include "json.h"
#include "mico.h"
void automation_poll(bool valid){(void)valid;}
uint32_t test_now;
/* Runtime harness does not launch RTOS workers; watchdog deadlines are tested
 * with the production monitor in test_watchdog.c. */
void watchdog_beat(watchdog_slot_t slot) { (void)slot; }
static feature_config_t stored;
uint32_t feature_config_revision(void) { return stored.revision; }
void feature_config_get(feature_config_t *out) { *out = stored;feature_config_limit_peers(out); }
bool feature_config_save(const feature_config_t *c) {
  if (c->revision != stored.revision)
    return false;
  stored = *c;
  stored.revision++;
  return true;
}
#ifdef FEATURE_DEVICE_A1
#include "a1.h"
char a1_id[13] = "aabbccddee01";
static unsigned local_controls;
static const char *test_a1_name = "Living Room A1";
void a1_config_get(a1_config_t *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%s", test_a1_name);
  out->speed = 60;
  out->brightness = 255;
  out->led_enabled = true;
}
mico_mutex_t a1_lock; a1_state_t a1_state;
void a1_state_get(a1_state_t *out) { memset(out, 0, sizeof(*out)); }
bool a1_control_local(const char *k, json_object *o) {
  local_controls++;
  (void)k;
  (void)o;
  return true;
}
#else
#include "m1.h"
char m1_id[13] = "aabbccddee02";
mico_mutex_t m1_lock; m1_state_t m1_state;
void m1_state_get(m1_state_t *out) { memset(out, 0, sizeof(*out)); }
#endif
static unsigned fail_runtime_object;
static json_object *runtime_new_object(void) {
  if (fail_runtime_object) { --fail_runtime_object; return NULL; }
  return json_object_new_object();
}
/* Execute the real common queue in the host owner; the device's session
 * drives these same pop/release operations after Paho callbacks unwind. */
#define lock common_queue_lock
#define unlock common_queue_unlock
#include "../common/mqtt/core.c"
#undef lock
#undef unlock
static ph_mqtt_core test_mqtt_queue;
static bool test_queue_initialized;
static unsigned test_queue_lock;
static void qlock(void*p){(void)p;assert(!test_queue_lock++);}
static void qunlock(void*p){(void)p;assert(test_queue_lock--==1);}
static bool qowner(void*p){(void)p;return true;}
static uint32_t qnow(void*p){(void)p;return test_now;}
static bool qpermit(void*p,size_t n){(void)p;(void)n;return true;}
static void *qalloc(void*p,size_t n){(void)p;return malloc(n);}
static void qfree(void*p,void*q){(void)p;free(q);}
uint32_t ph_mqtt_send_generation(void){
 if(!test_queue_initialized){ph_mqtt_hooks h={NULL,qlock,qunlock,qowner,qnow,qpermit,qalloc,qfree};
  assert(ph_mqtt_core_init(&test_mqtt_queue,&h));test_queue_initialized=true;}
 return test_mqtt_queue.generation;
}
int ph_mqtt_enqueue_send_generation(const char*t,const void*b,size_t n,unsigned qos,bool r,uint32_t g){
 ph_mqtt_send_generation();if(!g)return -1;
 return ph_mqtt_enqueue_generation(&test_mqtt_queue,PH_MQTT_SEND,t,strlen(t),b,n,qos,r,0,g)?0:-1;
}
#define json_object_new_object runtime_new_object
#include "../common/runtime.c"
#undef json_object_new_object
#define queued (test_mqtt_queue.count[PH_MQTT_SEND])
static void host_feature_mqtt_session(const mqtt_account_t *account){
 ph_mqtt_send_generation();assert(ph_mqtt_clear(&test_mqtt_queue));feature_mqtt_session(account);
}
static int host_feature_mqtt_service(int(*publish)(const char*,const char*,bool),bool connected){
 ph_mqtt_send_generation();
 if(connected){ph_mqtt_item*i;
  while((i=ph_mqtt_pop(&test_mqtt_queue,PH_MQTT_SEND))){
   int rc=publish(ph_mqtt_item_topic(i),ph_mqtt_item_payload(i),i->retained);
   ph_mqtt_item_release(&test_mqtt_queue,i);if(rc)return rc;
  }
 }
 return feature_mqtt_service(publish,connected);
}
#define feature_mqtt_session host_feature_mqtt_session
#define feature_mqtt_service host_feature_mqtt_service

/* Discovery is resumable now. Exercise its real cursor to completion rather
 * than assuming the first turn publishes (stage zero may only retire state).
 * 256 exceeds the maximum fixed topology's entity and domain steps; failure
 * to finish remains an assertion, not an unbounded harness wait. */
static void bootstrap_mqtt(int (*publish)(const char *, const char *, bool)) {
  for (unsigned step = 0; step < 256; ++step) {
    assert(feature_mqtt_service(publish, true) == 0);
    if (feature_mqtt_initial_ready()) return;
  }
  assert(!"MQTT bootstrap failed to complete in bounded owner turns");
}

static bool scoped_topic(const char *topic) {
 mqtt_account_t a=mqtt_snapshot();char prefix[160];mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",a.key);
 return !strncmp(topic,prefix,strlen(prefix));
}
json_object *automation_command(const char *a,json_object *p,const char **code,bool *handled){(void)a;(void)p;(void)code;*handled=false;return NULL;}
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char *a,json_object *p,const char **code){(void)a;(void)p;*code="unsupported";return NULL;}

static unsigned publications;
static int capture(const char *t, const char *b, bool r) {
  assert(t && b);
  (void)r;
  publications++;
  return 0;
}
static json_object *obj(const char *s) {
  json_object *o = json_tokener_parse(s);
  assert(o);
  return o;
}
static void transaction(const char *d, const char *s, unsigned code) {
  json_object *o = obj(s), *r = NULL;
  assert(feature_set(d, o, &r) == (int)code);
  json_object_put(o);
  assert(r);
  json_object_put(r);
}
static void binding(void) {
  transaction(
      "binding",
      "{\"peers\":[{\"mac\":\"11:22:33:44:55:66\",\"ip\":\"127.0.0.1\"}]}",
      200);
  char filters[12][FEATURE_TOPIC_MAX];
  assert(feature_mqtt_filters(filters,sizeof(filters)/sizeof(filters[0])) == 4);
  char root[FEATURE_TOPIC_MAX];
  root_for(&stored, 0, root);
#ifdef FEATURE_DEVICE_A1
  assert(!strcmp(root, "phicomm/v2/local/a1/aabbccddee01/linkage"));
#else
  assert(!strcmp(root, "phicomm/v2/local/a1/112233445566/linkage"));
#endif
  json_object *o = envelope(&stored, 0, "bound", 1);
  js(o, "src", stored.peers[0].id);
  js(o, "dst", LOCAL_ID);
  incoming(o, false, false, NULL);
  assert(feature_bound());
  json_object_put(o);
#ifdef FEATURE_DEVICE_A1
  char topic[FEATURE_TOPIC_MAX];
  const char *light_command = "{\"state\":\"ON\",\"brightness\":100}";
  assert(!feature_mqtt_input("phicomm/a1/aabbccddee01/light/set", light_command,
                             strlen(light_command), false));
  unsigned before = local_controls;
  uint32_t revision = stored.revision;
  snprintf(topic, sizeof(topic), "%s/light/set", root);
  assert(
      feature_mqtt_input(topic, light_command, strlen(light_command), false));
  assert(local_controls == before + 1 && stored.revision == revision);
  snprintf(topic, sizeof(topic), "%s/light/config/set", root);
  assert(feature_mqtt_input(topic, "{\"effect\":2}", 12, false));
  assert(stored.light.effect == 2 && stored.revision > revision);
#endif
  o = envelope(&stored, 0, "sample", 2);
  js(o, "src", stored.peers[0].id);
  js(o, "dst", LOCAL_ID);
  number(o, "boot", 7);
  feature_sample_t sample = {.value = {20, 50, 100, .1},
                             .sequence = 9,
                             .boot = 7,
                             .received_ms = 100,
                             .valid = true, .co2_supported=true, .co2_valid=true, .co2=1234, .co2_received_ms=100};
  test_now = 200;
  json_object_object_add(o, "body", sample_body(&sample, test_now));
  incoming(o, false, false, NULL);
  assert(samples[0].valid && samples[0].received_ms == 200);
  assert(samples[0].co2==1234 && feature_co2_fresh(&samples[0],200));
  assert(!feature_co2_fresh(&samples[0],20100));
  test_now = 300;
  incoming(o, false, false, NULL);
  assert(samples[0].received_ms == 200);
  js(o, "token", "wrong");
  number(json_object_object_get(o, "body"), "sequence", 10);
  incoming(o, false, false, NULL);
  assert(samples[0].sequence == 9);
  js(o, "token", "abababababababababababababababab");
  number(o, "boot", 8);
  number(json_object_object_get(o, "body"), "boot", 8);
  number(json_object_object_get(o, "body"), "sequence", 1);
  incoming(o, false, false, NULL);
  assert(samples[0].boot == 8);
  number(o, "boot", 7);
  number(json_object_object_get(o, "body"), "boot", 7);
  number(json_object_object_get(o, "body"), "sequence", 100);
  incoming(o, false, false, NULL);
  assert(samples[0].boot == 8);
  json_object_put(o);
  feature_local_sample(&sample);
  assert(!feature_mqtt_input("unrelated/topic", "{}", 2, false));
  bootstrap_mqtt(capture);
  assert(publications);
  transaction("binding", "{\"peers\":[]}", 200);
  assert(!feature_bound());
}
#ifdef FEATURE_DEVICE_A1
static void assert_group_quiet(int receiver) {
  assert(!queued && !group_members[0].pending);
  fd_set readable; FD_ZERO(&readable); FD_SET(receiver, &readable);
  struct timeval immediate = {0, 0};
  assert(select(receiver + 1, &readable, NULL, NULL, &immediate) == 0);
}
static void group_allocation_failure(int receiver, json_object *body) {
  feature_mqtt_service(capture, true);
  assert(!queued);
  /* A missing protocol envelope must never become a successful wire "null".
   * Keep the SDK's legitimate JSON-null serialization unchanged. */
  assert(!strcmp(json_object_to_json_string(NULL), "null"));
  assert(!send_udp("127.0.0.1", NULL));
  char topic[FEATURE_TOPIC_MAX];
  mqtt_account_t account = mqtt_snapshot();
  assert(mqtt_link_topic(&account, "a1", stored.members[0].id, LOCAL_ID,
                        "request", topic, sizeof(topic)));
  assert(!enqueue(topic, NULL, false));
  assert_group_quiet(receiver);

  for (unsigned mqtt_only = 0; mqtt_only < 2; mqtt_only++) {
    stored.group_force_mqtt = mqtt_only;
    fail_runtime_object = 1;
    assert(feature_group_control("fan", body));
    assert(!fail_runtime_object);
    assert_group_quiet(receiver);
    fail_runtime_object = 1;
    group_broadcast_config("light", body);
    assert(!fail_runtime_object);
    assert_group_quiet(receiver);
  }
  stored.group_force_mqtt = 0;
  group_members[0].lan = false;
  fail_runtime_object = 1;
  group_poll(&stored, test_now); /* Recovery probe root allocation fails. */
  assert(!fail_runtime_object);
  assert_group_quiet(receiver);
  group_members[0].lan = true;
  puts("Group envelope allocation failure emits no MQTT/UDP null payload");
}
static void group_transport(void) {
  transaction("binding",
              "{\"group\":\"room\",\"members\":[{\"mac\":\"112233445566\","
              "\"ip\":\"127.0.0.1\"}]}",
              200);
  memset(stored.group_token, 0xab, 16);
  configuration = stored;
  int receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(receiver >= 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(FEATURE_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(receiver, (struct sockaddr *)&a, sizeof(a)) == 0);
  udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(udp >= 0);
  group_members[0].joined = true;
  group_members[0].lan = true;
  json_object *b = obj("{\"speed\":60}");
  group_allocation_failure(receiver, b);
  assert(feature_group_control("fan", b));
  /* Retry ownership must not retain the caller's non-atomic JSON references.
   * The UDP/MQTT acknowledgement can destroy the pending tree concurrently. */
  assert(group_members[0].pending);
  number(b, "speed", 99);
  assert(integer(json_object_object_get(group_members[0].pending, "body"),
                 "speed") == 60);
  json_object_put(b);
  char wire[FEATURE_WIRE_MAX + 1];
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(receiver, &fds);
  struct timeval wait = {1, 0};
  assert(select(receiver + 1, &fds, NULL, NULL, &wait) == 1);
  int n = recv(receiver, wire, sizeof(wire), 0);
  assert(n > 0);
  json_object *o = parse(wire, n);
  assert(o && !strcmp(string(o, "op"), "group_control"));
  assert(integer(json_object_object_get(o, "body"), "speed") == 60);
  json_object_put(o);
  test_now += 1001;
  group_poll(&stored, test_now);
  assert(!group_members[0].pending && !group_members[0].lan &&
         group_members[0].failures == 1);
  assert(queued);
  feature_mqtt_service(capture, true);
  group_members[0].joined = true;
  group_members[0].retry_at = 0;
  group_poll(&stored, test_now);
  assert(group_members[0].pending);
  o = group_envelope(&stored, 0, "group_ack");
  js(o, "src", stored.members[0].id);
  js(o, "dst", LOCAL_ID);
  number(o, "id", group_members[0].request);
  incoming(o, true, false, "127.0.0.1");
  assert(!group_members[0].pending && !group_members[0].lan);
  group_poll(&stored, test_now);
  number(o, "id", group_members[0].request);
  incoming(o, true, false, "127.0.0.1");
  assert(group_members[0].lan);
  json_object_put(o);
  close(receiver);
  close(udp);
  udp = -1;
  transaction("binding", "{\"group\":\"\",\"members\":[]}", 200);
  puts("UDP group command, isolated timeout, MQTT fallback and two-probe "
       "recovery passed");
}
#endif

#ifdef FEATURE_DEVICE_A1
static void effective_controls(void) {
  feature_config_t c = stored;
  c.light.effect = EFFECT_SOLID;
  assert(install_config(&c));
  uint32_t pixels[32] = {0};
  sleep_state = (feature_sleep_state_t){0};
  feature_light_frame(pixels, 255, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 255);
  sleep_state.light_sleep = true;
  sleep_state.brightness = 0;
  feature_light_frame(pixels, 255, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 0);
  sleep_state.brightness = 16;
  feature_light_frame(pixels, 128, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 16);
  configuration.light.effect = EFFECT_FAN;
  for (unsigned i = 0; i < 32; i++)
    pixels[i] = 128;
  feature_light_frame(pixels, 128, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 16);
  sleep_state.fan_sleep = true;
  sleep_state.max_speed = 30;
  bool on;
  unsigned speed;
  feature_effective_fan(true, 100, &on, &speed);
  assert(on && speed == 30);
  /* A status copy must report the last applied controller result, never
     advance PID or recompute from a newly requested setting. */
  { bool applied,automatic;unsigned percentage,limit;
    __typeof__(pid) before=pid;bool valid=automatic_valid;
    uint32_t clock=test_now;
    feature_fan_snapshot(&applied,&percentage,&automatic,&limit);
    assert(applied==on && percentage==speed);
    assert(!memcmp(&before,&pid,sizeof(pid)) && automatic_valid==valid && test_now==clock);
    effective_on=false;effective_speed=17;
    feature_fan_snapshot(&applied,&percentage,&automatic,&limit);
    assert(!applied && percentage==17); /* requested state is not consulted */
    effective_on=on;effective_speed=speed;
  }
  c = stored;
  c.scheduled_off = 1;
  c.scheduled_valid = 1;
  c.last_event = 123;
  assert(install_config(&c));
  feature_effective_fan(true, 100, &on, &speed);
  assert(on && speed == 30); /* Old timer latch never overrides a new command. */
  feature_manual_fan(true);
  assert(!stored.scheduled_off && !stored.scheduled_valid &&
         stored.last_event == 123);
  sleep_state = (feature_sleep_state_t){0};
  configuration.light = stored.light;
  unsigned revision = stored.revision;
  for (unsigned i = 0; i < 1000; i++)
    feature_effective_fan(true, 60, &on, &speed);
  assert(stored.revision == revision);
  puts("Effective fan cap, persisted timer latch, manual resume and light "
       "sleep priority passed");
}
static void automatic_profiles(void) {
  feature_config_t c=stored;
  c.linkage.enabled=1;c.linkage.min_speed=10;c.linkage.max_speed=80;
  c.automatic.fallback_speed=90;c.sleep_count=1;
  c.sleeps[0].max_speed=35;c.automatic.night[0].min_speed=0;
  c.automatic.night[0].fallback_speed=0;
  assert(install_config(&c));data_valid=false;automatic_night=-1;
  bool on;unsigned speed;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==80);
  automatic_night=0;automatic_valid=false;automatic_immediate=true;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  feature_effective_fan(false,64,&on,&speed);assert(!on && speed==0);
  json_object *command=obj("{\"speed\":22}");
  assert(feature_fan_command(command)==2);json_object_put(command);
  assert(stored.sleeps[0].max_speed==22 && stored.linkage.max_speed==80);
  assert(feature_fan_limit()==22);
  automatic_night=-1;automatic_valid=false;automatic_immediate=true;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==80);
  c=stored;c.linkage.hysteresis_enabled=1;c.linkage.mode=0;
  c.linkage.fixed_speed=55;c.linkage.on_pm=50;c.linkage.target_pm=35;
  c.linkage.debounce_seconds=5;c.linkage.min_switch_seconds=10;
  assert(install_config(&c));data_valid=true;available=1;aggregate[2]=40;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  aggregate[2]=50;automatic_valid=false;test_now=1000;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  automatic_valid=false;test_now=6000;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==55);
  aggregate[2]=40;
  command=obj("{\"speed\":45}");assert(feature_fan_command(command)==2);json_object_put(command);
  feature_manual_fan(true);
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==45 && pid.gate_running);
  aggregate[2]=35;test_now=7000;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(speed==45);
  test_now=16000;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  /* Missing sensor overrides the gate; recovery cancels pending conditions. */
  data_valid=false;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(speed==45);
  data_valid=true;aggregate[2]=40;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  feature_manual_fan(false);assert(!pid.gate_running && !pid.switched);
  c=stored;c.linkage.enabled=0;c.linkage.hysteresis_enabled=0;c.sleep_count=0;assert(install_config(&c));
  puts("Independent day/night automatic ranges, zero fallback, power latch and active cap passed");
}
#endif
static char expected_key[MQTT_KEY_MAX];
static unsigned account_ha;
#ifdef FEATURE_DEVICE_A1
static const char *expected_device_name;
static unsigned named_discoveries;
static char discovery_ids[8][192], discovery_topics[8][FEATURE_TOPIC_MAX];
static bool compare_discovery_identity;
static int capture_discovery_name(const char *topic, const char *body, bool retained) {
  /* A previously advertised CO2 capability also needs a retained tombstone
   * after the preceding lifecycle scenario removed that peer. */
  if(!*body) {
    assert(retained && (strstr(topic,"scd40_") || strstr(topic,"_co2/config")));
    return 0;
  }
  assert(retained && named_discoveries < 8);
  json_object *o = obj(body), *device = json_object_object_get(o, "device");
  assert(!strcmp(string(device, "name"), expected_device_name));
  const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮",
                         "温度", "湿度", "PM2.5", "甲醛"};
  assert(!strcmp(string(o, "name"), names[named_discoveries]));
  assert(!strcmp(string(device, "model"), "A1 Air Purifier (EMW3080B)"));
  if (!named_discoveries) assert(!strcmp(string(o, "icon"), "mdi:air-purifier"));
  const char *id = json_object_get_string(json_object_array_get_idx(
      json_object_object_get(device, "identifiers"), 0));
  if (compare_discovery_identity) {
    assert(!strcmp(discovery_ids[named_discoveries], id));
    assert(!strcmp(discovery_topics[named_discoveries], topic));
  } else {
    snprintf(discovery_ids[named_discoveries], sizeof(discovery_ids[0]), "%s", id);
    snprintf(discovery_topics[named_discoveries], sizeof(discovery_topics[0]), "%s", topic);
  }
  named_discoveries++;
  json_object_put(o);
  return 0;
}
static unsigned sensor_entities, co2_entities, aux_entities;
static bool remove_sensor_entities;
static int capture_sensor_discovery(const char *topic, const char *body, bool retained) {
  assert(retained && strstr(topic,"homeassistant/"));
  if(remove_sensor_entities) { assert(!*body); sensor_entities++; return 0; }
  if(!*body) { assert(strstr(topic,"scd40_")); aux_entities++; return 0; }
  json_object *o=obj(body);
  if(!strcmp(string(o,"device_class"),"carbon_dioxide")) {
    assert(!strcmp(string(o,"unit_of_measurement"),"ppm"));
    assert(strstr(string(o,"value_template"),"co2")); co2_entities++;
  }
  if(strstr(string(o,"name"),"SCD40")) {
    assert(strstr(string(o,"value_template"),"scd40_")); aux_entities++;
    assert(!strstr(topic,"020000dd0002_scd40_"));
  }
  if(strstr(topic,"/sensor/")) {
    json_object *node=json_object_array_get_idx(json_object_object_get(o,"availability"),0);
    assert(!strcmp(string(node,"topic"),string(o,"state_topic")));
    assert(strstr(string(node,"value_template"),"valid"));
    assert(integer(o,"expire_after")==15);
  }
  sensor_entities++; json_object_put(o); return 0;
}
static void optional_sensor_discovery(void) {
  feature_config_t saved=configuration, config;
  feature_sample_t saved_samples[8]; memcpy(saved_samples,samples,sizeof(samples));
  feature_defaults(&config); config.peer_count=2;
  strcpy(config.peers[0].id,"020000dd0001"); strcpy(config.peers[1].id,"020000dd0002");
  configuration=config; memset(samples,0,sizeof(samples));
  samples[0].co2_supported=samples[1].co2_supported=true;
  char root[FEATURE_TOPIC_MAX];root_for(&config,0,root);
  assert(discovery_sensors(&config)==(1u|2u));
  discovery_virtual(capture_sensor_discovery,&config,root,false);
  assert(sensor_entities==19 && co2_entities==3 && aux_entities==6);
  /* Freshness does not remove registered entities. Layout cleanup removes all optional IDs. */
  remove_sensor_entities=true; sensor_entities=0;
  discovery_virtual(capture_sensor_discovery,&config,root,true);
  assert(sensor_entities==25);
  remove_sensor_entities=false;
  configuration=saved;memcpy(samples,saved_samples,sizeof(samples));
  puts("Optional HA sensors: mixed M1 capabilities, averages, independent availability and retained cleanup passed");
}
static void discovery_device_names(void) {
  feature_config_t config;
  feature_defaults(&config);
  config.peer_count = 1;
  char root[FEATURE_TOPIC_MAX];
  root_for(&config, 0, root);
  const char *names[] = {"Living Room A1", "Bedroom A1", ""};
  for (unsigned i = 0; i < 3; i++) {
    test_a1_name = names[i];
    expected_device_name = *names[i] ? names[i] : "Phicomm A1";
    named_discoveries = 0;
    compare_discovery_identity = i != 0;
    discovery_virtual(capture_discovery_name, &config, root, false);
    assert(named_discoveries == 8);
    feature_config_t saved = stored;
    stored = config;
    char parent[192], name[FEATURE_TOPIC_MAX];
    bool grouped;
    assert(feature_ha_device(parent, sizeof(parent), name, sizeof(name), &grouped));
    assert(!grouped && !strcmp(parent, discovery_ids[0]) &&
           !strcmp(name, expected_device_name));
    stored = saved;
  }
  strcpy(config.group, "客厅净化组");
  root_for(&config, 0, root);
  expected_device_name = config.group;
  named_discoveries = 0;
  compare_discovery_identity = false;
  discovery_virtual(capture_discovery_name, &config, root, false);
  assert(named_discoveries == 8);
  feature_config_t saved = stored;
  stored = config;
  char parent[192], name[FEATURE_TOPIC_MAX];
  bool grouped;
  assert(feature_ha_device(parent, sizeof(parent), name, sizeof(name), &grouped));
  assert(grouped && !strcmp(parent, discovery_ids[0]) && !strcmp(name, config.group));
  stored = saved;
  test_a1_name = names[0];
  puts("Bound A1 discovery uses Chinese names, purifier metadata and a shared filter parent");
}
#endif
static int capture_account(const char *t,const char *body,bool retained) {
  assert(t && body);
  if(!strncmp(t,"homeassistant/",14)) {
    assert(strstr(t,expected_key));
    unsigned slashes=0;for(const char *p=t;*p;++p)slashes+=*p=='/';
    assert(slashes==4); ++account_ha;
#ifdef FEATURE_DEVICE_A1
    assert(strstr(t,"/a1_000000000001_bound_"));
#endif
  } else {
    assert(strstr(t,expected_key));
    assert(!strncmp(t,"phicomm/",8));
  }
  (void)retained;return 0;
}
static void account_namespaces(void) {
#ifdef FEATURE_DEVICE_A1
  discovery_device_names();
  optional_sensor_discovery();
#endif
  mqtt_account_t alice,bob,legacy;
  char user[64];memset(user,'x',63);user[63]=0;
#ifdef FEATURE_DEVICE_A1
  const char *model="a1";
#else
  const char *model="m1";
#endif
  assert(mqtt_account_init(&alice,1,model,user,LOCAL_ID));
  assert(mqtt_account_init(&bob,1,model,"bob",LOCAL_ID));
  assert(mqtt_account_init(&legacy,0,model,"",LOCAL_ID));
  feature_defaults(&stored);strcpy(stored.group,"同名组");
  stored.peer_count=1;strcpy(stored.peers[0].id,"112233445566");stored.peers[0].kind=FEATURE_M1;stored.peers[0].confirmed=1;
  stored.member_count=1;strcpy(stored.members[0].id,"000000000001");
  configuration=stored;
  feature_mqtt_session(&alice);strcpy(expected_key,alice.key);
#ifdef FEATURE_DEVICE_A1
  char parent[192], parent_name[FEATURE_TOPIC_MAX], expected_parent[192];
  bool grouped;
  assert(feature_ha_device(parent, sizeof(parent), parent_name, sizeof(parent_name), &grouped));
  char encoded[FEATURE_TOPIC_MAX], parent_root[FEATURE_TOPIC_MAX];
  root_for(&stored, 0, parent_root);
  assert(mqtt_hex(encoded, sizeof(encoded), strrchr(parent_root, '/') + 1));
  assert(mqtt_format(expected_parent, sizeof(expected_parent), "v2_%s_group_%s", alice.key, encoded));
  assert(grouped && !strcmp(parent, expected_parent) && !strcmp(parent_name, stored.group));
#endif
  char filters[12][FEATURE_TOPIC_MAX],root[FEATURE_TOPIC_MAX];
  unsigned count=feature_mqtt_filters(filters,sizeof(filters)/sizeof(filters[0]));assert(count>=3);
  for(unsigned i=0;i<count;++i)assert(strstr(filters[i],alice.key));
  assert(feature_mqtt_filters_current(filters,count));
  stored.revision++;configuration=stored;
  assert(feature_mqtt_filters_current(filters,count));
  stored.peer_count=0;stored.group[0]=0;configuration=stored;
  assert(!feature_mqtt_filters_current(filters,count));
  stored.peer_count=1;strcpy(stored.group,"同名组");configuration=stored;
  root_for(&stored,0,root);assert(!strncmp(root,"phicomm/v2/",11));assert(strstr(root,alice.key));
  char t[FEATURE_TOPIC_MAX];mqtt_leaf(&alice,"binding/get",t,sizeof(t));
  assert(feature_mqtt_input(t,"{}",2,false));
  assert(queued);assert(feature_mqtt_service(capture_account,true)==0);
  bootstrap_mqtt(capture_account);
  mqtt_leaf(&alice,"binding/get",t,sizeof(t));assert(feature_mqtt_input(t,"{}",2,false));assert(queued);
  feature_mqtt_session(&bob);assert(!queued);
  assert(!feature_mqtt_input(t,"{}",2,false));
  json_object *o=obj("{}");assert(!enqueue(t,o,false));json_object_put(o);
  mqtt_leaf(&bob,"binding/get",t,sizeof(t));
  assert(feature_mqtt_input(t,"{}",2,true));assert(!queued); /* no retained RPC */
  feature_mqtt_session(&legacy);
  assert(!scoped_topic(t));
#ifdef FEATURE_DEVICE_A1
  assert(account_ha>0);
#endif
  puts("Long account namespaces, scoped RPC, stable HA owner and stale queue isolation passed");
}

/* The production limits are tested with canaries, including all eight M1
 * peers. Exercise every queued route with a maximum-length account/group. */
static char route_expected[FEATURE_TOPIC_MAX];
static unsigned route_calls;
static int capture_route(const char *topic,const char *body,bool retained){
 assert(!strcmp(topic,route_expected));assert(body && retained);route_calls++;return 0;
}
static void compact_resources(void){
 feature_config_t saved=stored;mqtt_account_t account=mqtt_snapshot();
 feature_defaults(&stored);configuration=stored;
 char user[64];memset(user,'x',63);user[63]=0;
 mqtt_account_t longest;assert(mqtt_account_init(&longest,1,account.model,user,LOCAL_ID));
 feature_mqtt_session(&longest);
#ifdef FEATURE_DEVICE_A1
 enum { capacity=FEATURE_MQTT_A1_FILTER_MAX };
#else
 enum { capacity=FEATURE_MQTT_M1_FILTER_MAX };
#endif
 struct { unsigned before;char topics[capacity][FEATURE_TOPIC_MAX];unsigned after; } filters;
 filters.before=0x12345678;filters.after=0x87654321;
 for(unsigned count=0;count<=FEATURE_MAX_PEERS;count++){
  stored.peer_count=count;configuration=stored;
  for(unsigned i=0;i<count;i++)snprintf(stored.peers[i].id,sizeof(stored.peers[i].id),"%012x",i+1);
#ifdef FEATURE_DEVICE_A1
  unsigned expected=3u+(count?1u:0u);
#else
  unsigned expected=count;
#endif
  assert(feature_mqtt_filters(filters.topics,capacity)==expected);
  assert(feature_mqtt_filters_current(filters.topics,expected));
  assert(!feature_mqtt_filters_current(filters.topics,capacity+1));
  if(expected){
   char saved_topics[sizeof(filters.topics)];memcpy(saved_topics,filters.topics,sizeof(saved_topics));
   assert(feature_mqtt_filters(filters.topics,expected-1)==FEATURE_MQTT_FILTER_ERROR);
   assert(!memcmp(saved_topics,filters.topics,sizeof(saved_topics)));
  }
  assert(filters.before==0x12345678 && filters.after==0x87654321);
 }
 stored.peer_count=0;memset(stored.group,'g',sizeof(stored.group)-1);stored.group[sizeof(stored.group)-1]=0;
 configuration=stored;
 char root[FEATURE_TOPIC_MAX];root_for(&stored,0,root);
 stored.group[0]=0;configuration=stored;
 const char *leaves[]={"result","binding/state","linkage/result","light/state","schedule/result","sleep/state","time/state"};
 json_object *body=obj("{\"ok\":true}");
 for(unsigned i=0;i<sizeof(leaves)/sizeof(leaves[0]);i++){
  assert(mqtt_format(route_expected,sizeof(route_expected),"%s/%s",root,leaves[i]));
  assert(enqueue(route_expected,body,true));
  assert(feature_mqtt_service(capture_route,true)==0 && !queued);
 }
 assert(mqtt_link_topic(&longest,"m1","112233445566",LOCAL_ID,"request",route_expected,sizeof(route_expected)));
 assert(enqueue(route_expected,body,true));assert(feature_mqtt_service(capture_route,true)==0);
 assert(mqtt_aux(&longest,"address",LOCAL_ID,route_expected,sizeof(route_expected)));
 for(unsigned i=0;i<7;i++)assert(enqueue(route_expected,body,true));
 assert(queued==1); /* retained state coalesces in the shared bounded queue */
 assert(feature_mqtt_service(capture_route,true)==0 && !queued);
 /* Reject oversized relative routes instead of truncating a destination. */
 char oversized[FEATURE_TOPIC_MAX],prefix[160];
 assert(mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",longest.key));
 strcpy(oversized,prefix);memset(oversized+strlen(prefix),'a',128);oversized[strlen(prefix)+128]=0;
 assert(!enqueue(oversized,body,true));
 assert(enqueue(route_expected,body,true));
 uint32_t stale=mqtt_generation;
 assert(ph_mqtt_clear(&test_mqtt_queue));mqtt_generation=ph_mqtt_send_generation();
 assert(ph_mqtt_enqueue_send_generation(route_expected,"{}",2,1,true,stale)!=0);
 unsigned before=route_calls;
 assert(feature_mqtt_service(capture_route,true)==0 && !queued && route_calls==before);
 json_object_put(body);stored=saved;configuration=saved;feature_mqtt_session(&account);
 puts("Compact resources: 4/4 filter bounds, full queue, long routes and stale session rejection passed");
}


static void four_peer_limit(void) {
  feature_config_t saved=stored;
  json_object *o=json_object_new_object(), *a=json_object_new_array(), *r=NULL;
#ifdef FEATURE_DEVICE_A1
  json_object_object_add(o,"peers",a);
#else
  json_object_object_add(o,"response_ids",a);
#endif
  for(unsigned i=1;i<=5;i++) {
    char mac[13];snprintf(mac,sizeof(mac),"%012x",i);
#ifdef FEATURE_DEVICE_A1
    json_object *p=json_object_new_object();js(p,"mac",mac);json_object_array_add(a,p);
#else
    char id[26];snprintf(id,sizeof(id),"%s:%s",mac,LOCAL_ID);
    json_object_array_add(a,json_object_new_string(id));
#endif
    if(i==4) {
      assert(feature_set("binding",o,&r)==200);json_object_put(r);r=NULL;
      assert(stored.peer_count==4 && configuration.peer_count==4);
    }
  }
  feature_config_t before=stored;
  assert(feature_set("binding",o,&r)==400);json_object_put(r);json_object_put(o);
  assert(!memcmp(&before,&stored,sizeof(stored)) && configuration.peer_count==4);
  o=envelope(&stored,0,"bind",1);js(o,"src","000000000005");js(o,"dst",LOCAL_ID);
  js(o,"token","abababababababababababababababab");
  incoming(o,false,false,NULL);json_object_put(o);
  assert(!memcmp(&before,&stored,sizeof(stored)) && configuration.peer_count==4);
  r=feature_json("binding");assert(integer(r,"max_peers")==4);json_object_put(r);
  /* Emulate an old eight-peer Flash record. Reads clamp without persisting. */
  stored.peer_count=FEATURE_STORAGE_PEERS;
  for(unsigned i=4;i<FEATURE_STORAGE_PEERS;i++)snprintf(stored.peers[i].id,sizeof(stored.peers[i].id),"%012x",i+1);
  before=stored;feature_config_t loaded;feature_config_get(&loaded);
  assert(loaded.peer_count==4 && !memcmp(loaded.peers,stored.peers,4*sizeof(loaded.peers[0])));
  assert(!memcmp(&before,&stored,sizeof(stored)));
  char filters[4][FEATURE_TOPIC_MAX];assert(feature_mqtt_filters(filters,4)==4);
  assert(!install_config(&stored)); /* Never install an oversized runtime map. */
  saved.revision=stored.revision;assert(install_config(&saved));
  puts("Four-peer boundary: four accepted, fifth rejected without mutation, inbound fifth ignored, legacy reads clamped");
}

#ifndef FEATURE_DEVICE_A1
#include "passive_scenarios.inc"
#endif
#ifdef FEATURE_DEVICE_A1
static void animation_speed(void) {
  feature_config_t original=configuration;
  uint32_t original_ms=test_now, normal[32], fast[32], slow[32];
  configuration.light.follow_fan=0;
  for(unsigned effect=EFFECT_BREATHE;effect<=EFFECT_RAINBOW;effect++){
    configuration.light.effect=effect;configuration.light.speed=0;test_now=3000;
    feature_light_frame(normal,255,true,0x1122ff);
    configuration.light.speed=5;feature_light_frame(fast,255,true,0x1122ff);
    assert(!memcmp(normal,fast,sizeof(normal))); /* zero-filled legacy byte */
    configuration.light.speed=10;test_now=1500;feature_light_frame(fast,255,true,0x1122ff);
    assert(!memcmp(normal,fast,sizeof(normal)));
    configuration.light.speed=1;test_now=15000;feature_light_frame(slow,255,true,0x1122ff);
    assert(!memcmp(normal,slow,sizeof(normal)));
    test_now=3000;feature_light_frame(slow,255,true,0x1122ff);
    assert(memcmp(normal,slow,sizeof(normal)));
  }
  configuration=original;test_now=original_ms;
  stored.light.speed=0;json_object *o=config_encode(&stored,"light");
  assert(json_object_get_int(json_object_object_get(o,"speed"))==5);json_object_put(o);
  transaction("light","{\"speed\":0}",400);
  transaction("light","{\"speed\":11}",400);
  transaction("light","{\"speed\":1.5}",400);
  transaction("light","{\"speed\":true}",400);
  transaction("light","{\"speed\":1}",200);assert(stored.light.speed==1);
  transaction("light","{\"speed\":10}",200);assert(stored.light.speed==10);
  transaction("light","{\"effect\":2}",200);assert(stored.light.speed==10);
  puts("Animation speed: legacy default, render timing, bounds and persistence passed");
}
#endif
int main(void) {
  service_init();
  feature_defaults(&stored);
  assert(feature_start() == 0);
  /* The production owner binds a queue generation before dispatching any
   * feature traffic. Model that lifecycle here too: generation zero rejects
   * stale/unbound producers by design. */
  mqtt_account_t initial_account = mqtt_snapshot();
  feature_mqtt_session(&initial_account);
  json_object *r = NULL;
#ifdef FEATURE_DEVICE_A1
  feature_sample_t readings[3] = {0};
  for (unsigned i=0;i<2;i++) { readings[i].valid=true;readings[i].received_ms=test_now;
    readings[i].value[0]=20+10*i;readings[i].value[1]=40+20*i;readings[i].value[2]=10+20*i;readings[i].value[3]=0.1f+0.2f*i; }
  json_object *mean=average_body(readings,3,test_now);
  assert(json_object_get_int(json_object_object_get(mean,"sample_count"))==2);
  assert(json_object_get_double(json_object_object_get(mean,"temperature"))==25);
  assert(json_object_get_double(json_object_object_get(mean,"pm25"))==20);
  json_object_put(mean);
  readings[2].co2_supported=readings[2].co2_valid=true;
  readings[2].co2=1400;readings[2].co2_received_ms=test_now;
  mean=average_body(readings,3,test_now);
  assert(json_object_get_int(json_object_object_get(mean,"co2_sample_count"))==1);
  assert(json_object_get_double(json_object_object_get(mean,"co2"))==1400);
  json_object_put(mean);
  mean=average_body(readings,3,test_now+180001);
  assert(!json_object_get_boolean(json_object_object_get(mean,"valid")));
  assert(!json_object_object_get(mean,"temperature"));json_object_put(mean);
  transaction("binding", "{\"peers\":[{\"mac\":\"bad\"}]}", 400);
  assert(!stored.peer_count);
  transaction("binding", "{\"group\":\"bad/#\"}", 400);
  transaction("binding",
              "{\"peers\":[{\"mac\":\"112233445566\",\"ip\":\"garbage\"}]}",
              400);
  binding();
#ifdef FEATURE_DEVICE_A1
  group_transport();
  effective_controls();
  automatic_profiles();
  animation_speed();
  transaction("linkage", "{\"hysteresis_enabled\":1,\"on_pm\":35,\"target_pm\":35}", 400);
  transaction("linkage", "{\"hysteresis_enabled\":1,\"metrics\":3,\"on_pm\":50,\"target_pm\":35,\"on_hcho\":0.08,\"target_hcho\":0.08}", 400);
  transaction("linkage", "{\"hysteresis_enabled\":1,\"metrics\":1,\"on_pm\":50,\"target_pm\":35}", 200);
  transaction("linkage", "{\"hysteresis_enabled\":2}", 400);
  transaction("linkage", "{\"metrics\":4,\"target_co2\":1000,\"on_co2\":900}", 400);
  transaction("linkage", "{\"metrics\":4,\"low_co2\":2000,\"high_co2\":800}", 400);
  transaction("linkage", "{\"metrics\":4,\"on_co2\":1500,\"target_co2\":1000}", 200);
  transaction("linkage", "{\"metrics\":1}", 200);
  transaction("linkage", "{\"hysteresis_enabled\":0}", 200);
  transaction("linkage", "{\"target_pm\":0}", 400);
  transaction("linkage", "{\"kp\":true}", 400);
  transaction("linkage", "{\"enabled\":1}", 200);
  transaction("linkage", "{\"enabled\":0}", 200);
  transaction("linkage", "{\"kp\":30}", 200);
  transaction("light", "{\"segments\":5}", 400);
  transaction("light", "{\"segment\":[{\"minimum\":10,\"maximum\":5}]}", 400);
  transaction("sleep", "{\"windows\":[{\"start\":120,\"end\":120}]}", 400);
  transaction(
      "schedule",
      "{\"events\":[{\"days\":127,\"minute\":480,\"on\":1,\"speed\":60}]}",
      200);
  transaction("time", "{\"timezone_minutes\":841}", 400);
  transaction("time", "{\"timezone_minutes\":480}", 200);
  transaction("time", "{\"revision\":0}", 409);
#endif
  account_namespaces();
#else
  passive_scenarios();
#endif
  compact_resources();
  four_peer_limit();
  (void)r;
  puts("runtime transactions, handshake, authentication and duplicate "
       "freshness passed");
  return 0;
}
