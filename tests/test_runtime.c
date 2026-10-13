#include "../common/phicomm_features.h"
#include "json.h"
#include "mico.h"
uint32_t test_now;
static feature_config_t stored;
void feature_config_get(feature_config_t *out) { *out = stored; }
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
void a1_config_get(a1_config_t *out) {
  memset(out, 0, sizeof(*out));
  out->speed = 60;
  out->brightness = 255;
  out->led_enabled = true;
}
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
void m1_state_get(m1_state_t *out) { memset(out, 0, sizeof(*out)); }
#endif
#include "../common/runtime.c"
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
  assert(feature_mqtt_filters(filters) == 3);
  char root[FEATURE_TOPIC_MAX];
  root_for(&stored, 0, root);
#ifdef FEATURE_DEVICE_A1
  assert(!strcmp(root, "aa:bb:cc:dd:ee:01:11:22:33:44:55:66"));
#else
  assert(!strcmp(root, "11:22:33:44:55:66:aa:bb:cc:dd:ee:02"));
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
                             .valid = true};
  test_now = 200;
  json_object_object_add(o, "body", sample_body(&sample, test_now));
  incoming(o, false, false, NULL);
  assert(samples[0].valid && samples[0].received_ms == 200);
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
  feature_mqtt_service(capture, true);
  assert(publications);
  transaction("binding", "{\"peers\":[]}", 200);
  assert(!feature_bound());
}
#ifdef FEATURE_DEVICE_A1
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
  assert(feature_group_control("fan", b));
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
#ifndef FEATURE_DEVICE_A1
static void proxy_transport(void) {
  transaction("binding",
              "{\"peers\":[{\"mac\":\"112233445566\",\"ip\":\"127.0.0.1\"}]}",
              200);
  stored.peers[0].confirmed = 1;
  configuration = stored;
  peers[0].lan = true;
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
  transaction("light", "{\"effect\":4,\"request_id\":\"proxy-1\"}", 202);
  assert(peers[0].rpc);
  char wire[FEATURE_WIRE_MAX + 1];
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(receiver, &fds);
  struct timeval wait = {1, 0};
  assert(select(receiver + 1, &fds, NULL, NULL, &wait) == 1);
  int n = recv(receiver, wire, sizeof(wire), 0);
  assert(n > 0);
  json_object *o = parse(wire, n);
  assert(o && !strcmp(string(o, "op"), "config_set") &&
         !strcmp(string(o, "domain"), "light"));
  assert(integer(json_object_object_get(o, "body"), "effect") == 4);
  json_object_put(o);
  test_now += 1001;
  rpc_poll(&stored, 0, test_now);
  assert(!peers[0].rpc && !peers[0].lan && queued);
  feature_mqtt_service(capture, true);
  o = envelope(&stored, 0, "config_result", 1);
  js(o, "src", stored.peers[0].id);
  js(o, "dst", LOCAL_ID);
  json_object_object_add(o, "body",
                         obj("{\"code\":200,\"request_id\":\"proxy-1\"}"));
  incoming(o, false, false, NULL);
  assert(remote_result && integer(remote_result, "code") == 200);
  json_object_put(o);
  assert(stored.light.effect == EFFECT_FAN);
  close(receiver);
  close(udp);
  udp = -1;
  transaction("binding", "{\"peers\":[]}", 200);
  puts(
      "M1 LAN configuration RPC, MQTT fallback and asynchronous result passed");
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
  c = stored;
  c.scheduled_off = 1;
  c.scheduled_valid = 1;
  c.last_event = 123;
  assert(install_config(&c));
  feature_effective_fan(true, 100, &on, &speed);
  assert(!on);
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
#endif
int main(void) {
  feature_defaults(&stored);
  assert(feature_start() == 0);
  json_object *r = NULL;
  transaction("binding", "{\"peers\":[{\"mac\":\"bad\"}]}", 400);
  assert(!stored.peer_count);
  transaction("binding", "{\"group\":\"bad/#\"}", 400);
  transaction("binding",
              "{\"peers\":[{\"mac\":\"112233445566\",\"ip\":\"garbage\"}]}",
              400);
  binding();
#ifndef FEATURE_DEVICE_A1
  proxy_transport();
#endif
#ifdef FEATURE_DEVICE_A1
  group_transport();
  effective_controls();
  transaction("linkage", "{\"target_pm\":0}", 400);
  transaction("linkage", "{\"kp\":true}", 400);
  transaction("linkage", "{\"enabled\":1}", 409);
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
  (void)r;
  puts("runtime transactions, handshake, authentication and duplicate "
       "freshness passed");
  return 0;
}
