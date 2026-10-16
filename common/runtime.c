#include "watchdog.h"
#include "automation.h"
#include "phicomm_features.h"
#include "sntp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FEATURE_DEVICE_A1
#include "a1.h"
#define LOCAL_ID a1_id
#else
#include "m1.h"
#define LOCAL_ID m1_id
#endif
#include "config_json.c"
#include "engine.c"
static mico_mutex_t feature_lock;
static mico_mutex_t namespace_lock;
static mqtt_account_t feature_account;
static int publish_error;
static mqtt_account_t mqtt_snapshot(void) {
  mqtt_account_t a;
  mico_rtos_lock_mutex(&namespace_lock); a = feature_account;
  mico_rtos_unlock_mutex(&namespace_lock); return a;
}
static bool scoped_topic(const char *topic) {
  mqtt_account_t a=mqtt_snapshot();char prefix[160];mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",a.key);
  return !strncmp(topic,prefix,strlen(prefix));
}

static feature_config_t configuration;
static feature_sample_t samples[8], local_sample;
static feature_pid_t pid;
static uint32_t boot_id, serial, announce_at, handshake_at;
static unsigned handshake_cursor;
#ifdef FEATURE_DEVICE_A1
static uint32_t tick_at;
#endif
static bool automatic_on, automatic_valid, scheduled_on, schedule_valid,
    clock_ok;
static unsigned automatic_speed, scheduled_speed;
static bool effective_on;

static feature_sleep_state_t sleep_state;
static float aggregate[4];
static bool data_valid;
static unsigned available;
typedef struct {
  uint32_t request, sent, recover_at, mqtt_sent, retired_boot[4];
  unsigned successes;
  bool lan, pending;
  char ip[16];
  json_object *rpc;
  uint32_t rpc_sent, rpc_request, last_seen;
#ifndef FEATURE_DEVICE_A1
  uint8_t session_token[16];
  bool session_valid;
#endif
} peer_runtime_t;
static peer_runtime_t peers[8];
static struct {
  char topic[FEATURE_TOPIC_MAX];
  char *body;
  bool retained;
} queue[4];
static unsigned queued, queued_bytes;
static uint32_t member_seen[8];
static uint32_t mqtt_revision;
static bool mqtt_online;
#ifdef FEATURE_DEVICE_A1
static uint32_t mqtt_tick;
static bool was_coordinator;
static char published_root[FEATURE_TOPIC_MAX], published_ids[8][13];
static feature_config_t *published_layout;
static unsigned published_count;
#endif
static int udp = -1;
static void lock(void) { mico_rtos_lock_mutex(&feature_lock); }
static void unlock(void) { mico_rtos_unlock_mutex(&feature_lock); }
static uint32_t now_ms(void) { return mico_rtos_get_time(); }
static unsigned next_request(void) {
  lock();
  unsigned id = ++serial;
  if (!id)
    id = ++serial;
  unlock();
  return id;
}
static bool peer_current(const feature_config_t *c, unsigned i) {
  return i < configuration.peer_count && i < c->peer_count &&
         !strcmp(configuration.peers[i].id, c->peers[i].id) &&
         !memcmp(configuration.peers[i].token, c->peers[i].token, 16);
}

static bool enqueue(const char *t, json_object *o, bool retain) {
  if (!o || !scoped_topic(t)) return false;
  const char *s = json_object_to_json_string(o);
  if (!s) return false;
  size_t n = strlen(s);
  if (n > 8192)
    return false;
  char *b = malloc(n + 1);
  if (!b)
    return false;
  strcpy(b, s);
  lock();
  if (queued == 4 || queued_bytes + n + 1 > 4096) {
    unlock();
    free(b);
    return false;
  }
  mqtt_format(queue[queued].topic, FEATURE_TOPIC_MAX, "%s", t);
  queue[queued].body = b;
  queue[queued].retained = retain;
  queued++;
  queued_bytes += n + 1;
  unlock();
  return true;
}
static json_object *parse(const void *data, size_t n) {
  if (!n || n > 8192 || memchr(data, 0, n))
    return NULL;
  for(size_t i=0;i+6<=n;i++)if(!memcmp((const char *)data+i,"\\u0000",6))return NULL;
  struct json_tokener *t = json_tokener_new();
  if (!t)
    return NULL;
  json_object *o = json_tokener_parse_ex(t, data, n);
  bool ok = t->err == json_tokener_success && o &&
            json_object_is_type(o, json_type_object);
  int used = t->char_offset;
  while (used < (int)n && ((const char *)data)[used] <= ' ')
    used++;
  ok = ok && used == (int)n;
  json_tokener_free(t);
  if (ok) {
    json_object_object_foreach(o, key, value) {
      UNUSED_PARAMETER(key);
      if (!value)
        ok = false;
    }
  }
  if (!ok) {
    if (o)
      json_object_put(o);
    return NULL;
  }
  return o;
}
static const char *topic_domain(const char *d) {
  return !strcmp(d, "light") ? "light/config" : d;
}
static unsigned integer(json_object *o, const char *k) {
  return json_object_get_int(json_object_object_get(o, k));
}
static const char *string(json_object *o, const char *k) {
  json_object *v = json_object_object_get(o, k);
  return v && json_object_is_type(v, json_type_string)
             ? json_object_get_string(v)
             : "";
}
static void number(json_object *o, const char *k, unsigned n) {
  json_build_object_add(o, k, json_object_new_int(n));
}
static void token_text(const uint8_t t[16], char out[33]) {
  for (unsigned i = 0; i < 16; i++)
    sprintf(out + i * 2, "%02x", t[i]);
}
static bool token_read(const char *s, uint8_t out[16]) {
  if (strlen(s) != 32)
    return false;
  for (unsigned i = 0; i < 16; i++) {
    unsigned v;
    char pair[3] = {s[i * 2], s[i * 2 + 1], 0};
    if (strspn(pair, "0123456789abcdef") != 2 || sscanf(pair, "%x", &v) != 1)
      return false;
    out[i] = v;
  }
  return true;
}
static bool new_token(uint8_t t[16]) {
  if (MicoRandomNumberRead(t, 16) != kNoErr)
    return false;
  unsigned n = 0;
  for (unsigned i = 0; i < 16; i++)
    n |= t[i];
  return n != 0;
}
static void root_for(const feature_config_t *c,unsigned i,char out[FEATURE_TOPIC_MAX]) {
  mqtt_account_t a=mqtt_snapshot();
#ifdef FEATURE_DEVICE_A1
  (void)i;
  if(c->group[0]) {char hex[67];mqtt_hex(hex,sizeof(hex),c->group);mqtt_format(out,FEATURE_TOPIC_MAX,"phicomm/v2/%s/group/g_%s",a.key,hex);}
  else mqtt_format(out,FEATURE_TOPIC_MAX,"%s/linkage",a.base);
#else
  mqtt_format(out,FEATURE_TOPIC_MAX,"phicomm/v2/%s/a1/%s/linkage",a.key,c->peers[i].id);
#endif
}
static json_object *envelope(const feature_config_t *c, unsigned i,
                             const char *op, unsigned request) {
  json_object *o = json_object_new_object();
  if (!o)
    return NULL;
  number(o, "v", 1);
  js(o, "src", LOCAL_ID);
  js(o, "dst", c->peers[i].id);
  number(o, "boot", boot_id);
  number(o, "id", request);
  number(o, "revision", c->revision);
  js(o, "op", op);
  char token[33];
#ifdef FEATURE_DEVICE_A1
  token_text(c->peers[i].token, token);
#else
  lock(); token_text(peers[i].session_token, token); unlock();
#endif
  js(o, "token", token);
#ifdef FEATURE_DEVICE_A1
  js(o, "group", c->group);
  number(o, "sensor_count", c->peer_count);
#endif
  return o;
}
static bool send_udp(const char *ip, json_object *o) {
  if (!o || udp < 0 || !ip[0])
    return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(FEATURE_PORT);
  a.sin_addr.s_addr = inet_addr(ip);
  if (a.sin_addr.s_addr == INADDR_NONE || a.sin_addr.s_addr == 0)
    return false;
  const char *b = json_object_to_json_string(o);
  if (!b) return false;
  /* UDP replies also run on the 6 KiB binding task. Keep packet storage
   * off that stack, which already holds the worker and protocol frames. */
  char *wire = malloc(FEATURE_WIRE_MAX);
  if (!wire) return false;
  size_t n = 0;
  bool quoted = false, escape = false;
  for (; *b; b++) {
    char ch = *b;
    if (!quoted && (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'))
      continue;
    if (n == FEATURE_WIRE_MAX) { free(wire); return false; }
    wire[n++] = ch;
    if (escape)
      escape = false;
    else if (quoted && ch == '\\')
      escape = true;
    else if (ch == '"')
      quoted = !quoted;
  }
  bool sent = sendto(udp, wire, n, 0, (struct sockaddr *)&a, sizeof(a)) == (int)n;
  free(wire);
  return sent;
}
static bool send_peer(const feature_config_t *c, unsigned i, json_object *o,
                      bool mqtt_only) {
  char topic[FEATURE_TOPIC_MAX];
  bool lan;
  char ip[16];
  lock();
  if (!peer_current(c, i)) {
    unlock();
    return false;
  }
  lan = peers[i].lan;
  strcpy(ip, c->peers[i].ip[0] ? c->peers[i].ip : peers[i].ip);
  unlock();
  if (!mqtt_only && !c->peers[i].force_mqtt && lan && send_udp(ip, o))
    return true;
  mqtt_account_t account = mqtt_snapshot();
  if (!mqtt_link_topic(&account,
#ifdef FEATURE_DEVICE_A1
      "m1",c->peers[i].id,LOCAL_ID,"request",
#else
      "a1",c->peers[i].id,LOCAL_ID,"response",
#endif
      topic,sizeof(topic))) return false;
  return enqueue(topic, o, false);
}
static void group_config_changed(const feature_config_t *,
                                 const feature_config_t *);
static bool install_config(feature_config_t *c) {
  for (unsigned i = 0; i < c->peer_count; i++) {
#ifdef FEATURE_DEVICE_A1
    unsigned n = 0;
    for (unsigned j = 0; j < 16; j++)
      n |= c->peers[i].token[j];
    if (!n) {
      c->peers[i].confirmed = 0;
      if (!new_token(c->peers[i].token))
        return false;
    }
#endif
  }
#ifndef FEATURE_DEVICE_A1
  /* Persist response permissions only, never negotiated LAN sessions. */
  for (unsigned i = 0; i < c->peer_count; i++) {
    memset(c->peers[i].token, 0, 16);
    c->peers[i].confirmed = 0;
    c->peers[i].ip[0] = 0;
  }
#endif
  if (!feature_config_save(c))
    return false;
  c->revision++;
  struct mapping {
    feature_sample_t samples[8];
    peer_runtime_t peers[8];
  } *next = calloc(1, sizeof(*next));
  lock();
  if (next) {
    for (unsigned i = 0; i < c->peer_count; i++)
      for (unsigned j = 0; j < configuration.peer_count; j++)
        if (!strcmp(c->peers[i].id, configuration.peers[j].id) &&
            !memcmp(c->peers[i].token, configuration.peers[j].token, 16)) {
          next->samples[i] = samples[j];
          next->peers[i] = peers[j];
          break;
        }
    for (unsigned j = 0; j < 8; j++) {
      bool retained = false;
      for (unsigned i = 0; i < 8; i++)
        retained |= next->peers[i].rpc && next->peers[i].rpc == peers[j].rpc;
      if (!retained && peers[j].rpc)
        json_object_put(peers[j].rpc);
    }
    memcpy(samples, next->samples, sizeof(samples));
    memcpy(peers, next->peers, sizeof(peers));
  } else {
    for (unsigned i = 0; i < 8; i++)
      if (peers[i].rpc)
        json_object_put(peers[i].rpc);
    memset(samples, 0, sizeof(samples));
    memset(peers, 0, sizeof(peers));
  }
  group_config_changed(&configuration, c);
  configuration = *c;
  memset(&pid, 0, sizeof(pid));
  unlock();
  free(next);
  return true;
}
static json_object *sample_body(const feature_sample_t *s, uint32_t now) {
  json_object *o = json_object_new_object();
  if (!o)
    return NULL;
  number(o, "sequence", s->sequence);
  number(o, "boot", s->boot);
  number(o, "age_ms", s->age_ms + (uint32_t)(now - s->received_ms));
  json_build_object_add(o, "valid",
                         json_object_new_boolean(feature_sample_fresh(s, now)));
  const char *keys[] = {"temperature", "humidity", "pm25", "formaldehyde"};
  for (unsigned i = 0; i < 4; i++)
    json_build_object_add(o, keys[i], json_object_new_double(s->value[i]));
  return o;
}
#ifdef FEATURE_DEVICE_A1
/* Caller supplies a stable snapshot. Missing/stale data is never a zero reading. */
static json_object *average_body(const feature_sample_t *current, unsigned count, uint32_t now) {
  float mean[4];
  unsigned n = feature_aggregate(current, count, now, STAT_MEAN, mean, NULL);
  json_object *o = json_object_new_object();
  number(o, "sample_count", n);
  number(o, "configured_count", count);
  json_build_object_add(o, "valid", json_object_new_boolean(n != 0));
  const char *keys[] = {"temperature", "humidity", "pm25", "formaldehyde"};
  for (unsigned i = 0; i < 4; ++i)
    if (n) json_build_object_add(o, keys[i],json_object_new_double(mean[i]));
    else json_object_object_add(o, keys[i], NULL);
  return o;
}
#endif
#include "group.c"
#ifndef FEATURE_DEVICE_A1
/* Linkage is a passive, read-only channel. Replies always use the configured
 * peer topic or the actual UDP sender; a payload cannot select a reply topic. */
static void passive_incoming(json_object *o, bool lan, bool retained, const char *ip) {
  if (retained || integer(o, "v") != 1 || strcmp(string(o, "dst"), LOCAL_ID)) return;
  feature_config_t *c = malloc(sizeof(*c));
  if (!c) return;
  feature_config_get(c);
  unsigned i;
  for (i = 0; i < c->peer_count; i++)
    if (!strcmp(c->peers[i].id, string(o, "src"))) break;
  if (i == c->peer_count) { free(c); return; }
  const char *op = string(o, "op");
  uint8_t token[16];
  if (!token_read(string(o, "token"), token)) { free(c); return; }
  bool handshake = !lan && !strcmp(op, "bind");
  if (!handshake && strcmp(op, "probe") && strcmp(op, "read")) { free(c); return; }
  lock();
  bool permitted = peer_current(c, i);
  if (permitted && handshake) {
    memcpy(peers[i].session_token, token, 16);
    peers[i].session_valid = true;
  } else permitted = permitted && peers[i].session_valid &&
                      !memcmp(peers[i].session_token, token, 16);
  feature_sample_t sample = local_sample;
  unlock();
  if (permitted) {
    json_object *r = envelope(c, i, handshake ? "bound" : !strcmp(op,"probe") ? "pong" : "sample", integer(o,"id"));
    m1_state_t *state = malloc(sizeof(*state));
    if (!r || !state) { if (r) json_object_put(r); free(state); free(c); return; }
    m1_state_get(state);
    js(r, "ip", state->ip);
    free(state);
    if (!strcmp(op,"read")) json_build_object_add(r,"body",sample_body(&sample,now_ms()));
    if (lan) send_udp(ip,r); else send_peer(c,i,r,true);
    json_object_put(r);
  }
  free(c);
}
#endif
static void incoming(json_object *o, bool lan, bool retained, const char *ip) {
#ifndef FEATURE_DEVICE_A1
  passive_incoming(o, lan, retained, ip);
  return;
#else
  if (retained || integer(o, "v") != 1 || strcmp(string(o, "dst"), LOCAL_ID))
    return;
  if (group_input(o, lan, retained, ip))
    return;
  char src[13];
  if (!feature_mac(string(o, "src"), src) || !strcmp(src, LOCAL_ID))
    return;
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return;
  feature_config_get(c);
  unsigned i;
  for (i = 0; i < c->peer_count; i++)
    if (!strcmp(c->peers[i].id, src))
      break;
  const char *op = string(o, "op");
  if (!strcmp(op, "bind") && !lan) {
    uint8_t token[16];
    if (!token_read(string(o, "token"), token))
      goto done;
    if (i == c->peer_count) {
      if (i == 8)
        goto done;
      memset(&c->peers[i], 0, sizeof(c->peers[i]));
      strcpy(c->peers[i].id, src);
      c->peer_count++;
      c->peers[i].timeout_ms = 1000;
      c->peers[i].recovery_seconds = 60;
      c->peers[i].recovery = 1;
#ifdef FEATURE_DEVICE_A1
      c->peers[i].kind = FEATURE_M1;
#else
      c->peers[i].kind = FEATURE_A1;
#endif
    } else if (c->peers[i].confirmed && memcmp(c->peers[i].token, token, 16))
      goto done;
    memcpy(c->peers[i].token, token, 16);
    c->peers[i].confirmed = 1;
#ifndef FEATURE_DEVICE_A1
    if (!string_field(o, "group", c->peers[i].group, sizeof(c->peers[i].group)))
      goto done;
    c->peers[i].sensor_count = integer(o, "sensor_count");
#endif
    if (!install_config(c))
      goto done;
    json_object *r = envelope(c, i, "bound", integer(o, "id"));
    send_peer(c, i, r, true);
    json_object_put(r);
    goto done;
  }
  if (i == c->peer_count)
    goto done;
  char expected[33];
  token_text(c->peers[i].token, expected);
  if (strcmp(expected, string(o, "token")))
    goto done;
  if (!strcmp(op, "bound") && !lan) {
    const char *address = string(o,"ip");
    if (strlen(address)<16 && inet_addr(address)!=INADDR_NONE && inet_addr(address)!=0) {
      lock(); strcpy(peers[i].ip,address); unlock();
    }
    if (!c->peers[i].confirmed) {
      c->peers[i].confirmed = 1;
#ifndef FEATURE_DEVICE_A1
      string_field(o, "group", c->peers[i].group, sizeof(c->peers[i].group));
      c->peers[i].sensor_count = integer(o, "sensor_count");
#endif
      install_config(c);
    }
    goto done;
  }
  if (!strcmp(op, "unbind")) {
    memmove(&c->peers[i], &c->peers[i + 1],
            (c->peer_count - i - 1) * sizeof(c->peers[0]));
    c->peer_count--;
    install_config(c);
    goto done;
  }
  if (!c->peers[i].confirmed)
    goto done;

  if (lan) {
    lock();
    if (!peer_current(c, i)) {
      unlock();
      goto done;
    }
    if (ip && ip[0])
      strcpy(peers[i].ip, ip);
    if (integer(o, "id") == peers[i].request && peers[i].pending) {
      peers[i].pending = false;
      peers[i].successes++;
      if (peers[i].successes >= 2)
        peers[i].lan = true;
    }
    unlock();
  }
  if (!strcmp(op, "config_get") || !strcmp(op, "config_set")) {
#ifdef FEATURE_DEVICE_A1
    const char *d = string(o, "domain");
    json_object *body = NULL;
    if (!strcmp(op, "config_set")) {
      feature_set(d, json_object_object_get(o, "body"), &body);
      json_object *r = envelope(c, i, "config_result", integer(o, "id"));
      js(r, "domain", d);
      if (body)
        json_build_object_add(r, "body", body);
      if (lan)
        send_udp(ip, r);
      else
        send_peer(c, i, r, true);
      json_object_put(r);
    }
    body = feature_json(d);
    if (body) {
      json_object *r = envelope(c, i, "config_state", integer(o, "id"));
      js(r, "domain", d);
      json_build_object_add(r, "body", body);
      if (lan)
        send_udp(ip, r);
      else
        send_peer(c, i, r, true);
      json_object_put(r);
    }
#endif
  } else if (!strcmp(op, "config_state") || !strcmp(op, "config_result")) {
    lock();
    json_object *pending = NULL;
    if (!peer_current(c, i)) {
      unlock();
      goto done;
    }
    if (peers[i].rpc_request == integer(o, "id")) {
      pending = peers[i].rpc;
      peers[i].rpc = NULL;
    }
    unlock();
    if (pending)
      json_object_put(pending);
  } else if (!strcmp(op, "probe") || !strcmp(op, "read")) {
    json_object *r = envelope(c, i, !strcmp(op, "probe") ? "pong" : "sample",
                              integer(o, "id"));
    if (!strcmp(op, "read")) {
      lock();
      feature_sample_t s = local_sample;
      unlock();
      json_build_object_add(r, "body", sample_body(&s, now_ms()));
    }
    if (lan)
      send_udp(ip, r);
    else
      send_peer(c, i, r, true);
    json_object_put(r);
  } else if (!strcmp(op, "sample")) {
    json_object *b = json_object_object_get(o, "body");
    feature_sample_t s;
    memset(&s, 0, sizeof(s));
    const char *keys[] = {"temperature", "humidity", "pm25", "formaldehyde"};
    bool valid = true;
    for (unsigned k = 0; k < 4; k++) {
      json_object *v = json_object_object_get(b, keys[k]);
      if (!v || (!json_object_is_type(v, json_type_double) &&
                 !json_object_is_type(v, json_type_int)))
        valid = false;
      s.value[k] = json_object_get_double(v);
    }
    s.sequence = integer(b, "sequence");
    s.boot = integer(b, "boot");
    s.age_ms = integer(b, "age_ms");
    s.received_ms = now_ms();
    s.valid = valid && integer(b, "valid");
    lock();
    if (!peer_current(c, i)) {
      unlock();
      goto done;
    }
    bool retired = false;
    for (unsigned k = 0; k < 4; k++)
      retired |= peers[i].retired_boot[k] && peers[i].retired_boot[k] == s.boot;
    if (s.boot != integer(o, "boot"))
      retired = true;
    if (!retired && (s.boot != samples[i].boot ||
                     (int32_t)(s.sequence - samples[i].sequence) > 0)) {
      if (samples[i].boot && s.boot != samples[i].boot) {
        memmove(peers[i].retired_boot + 1, peers[i].retired_boot,
                3 * sizeof(uint32_t));
        peers[i].retired_boot[0] = samples[i].boot;
      }
      samples[i] = s;
    }
    unlock();
  }
done:
  free(c);
#endif
}
bool feature_bound(void) {
  lock();
  bool b = false;
#ifdef FEATURE_DEVICE_A1
  b = configuration.group[0] != 0;
#endif
  for (unsigned i = 0; i < configuration.peer_count; i++)
#ifdef FEATURE_DEVICE_A1
    b |= configuration.peers[i].confirmed;
#else
    b |= peers[i].session_valid;
#endif
  unlock();
  return b;
}
#ifdef FEATURE_DEVICE_A1
static bool data_bound(void) {
  lock();
  bool bound = false;
  for (unsigned i = 0; i < configuration.peer_count; i++)
    bound |= configuration.peers[i].confirmed;
  unlock();
  return bound;
}
#endif
bool feature_is_coordinator(void) {
  lock();
  bool leader = true;
  uint32_t now = now_ms();
  if (configuration.group[0])
    for (unsigned i = 0; i < configuration.member_count; i++)
      if (strcmp(configuration.members[i].id, LOCAL_ID) < 0 && member_seen[i] &&
          (uint32_t)(now - member_seen[i]) < 15000)
        leader = false;
  unlock();
  return leader;
}
void feature_local_sample(const feature_sample_t *s) {
  lock();
  local_sample = *s;
  unlock();
}
void feature_clock_valid(void) { clock_ok = true; }
json_object *feature_json(const char *d) {

  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return NULL;
  feature_config_get(c);
  json_object *o;
#ifdef FEATURE_DEVICE_A1
  if (!strcmp(d, "average")) {
    feature_sample_t current[8];
    lock(); memcpy(current, samples, sizeof(current)); unlock();
    o = average_body(current, c->peer_count, now_ms());
  } else
#endif
  if (!strcmp(d, "status")) {
    o = json_object_new_object();
    lock();
    number(o, "available", available);
    json_build_object_add(o, "data_valid",
                           json_object_new_boolean(data_valid));
    json_build_object_add(o, "clock_valid", json_object_new_boolean(clock_ok));
    json_build_object_add(o, "automatic_on",
                           json_object_new_boolean(automatic_on));
    number(o, "automatic_speed", automatic_speed);
    json_object *a = json_object_new_array();
    for (unsigned i = 0; i < c->peer_count; i++) {
      json_object *p = json_object_new_object();
      js(p, "mac", c->peers[i].id);
#ifndef FEATURE_DEVICE_A1
      json_build_object_add(p,"session_ready",json_object_new_boolean(peers[i].session_valid));
#endif
      js(p, "ip", c->peers[i].ip[0] ? c->peers[i].ip : peers[i].ip);
      js(p, "transport",
         peers[i].lan && !c->peers[i].force_mqtt ? "lan" : "mqtt");
      json_build_object_add(p, "pending",
                             json_object_new_boolean(peers[i].pending));
      json_build_object_add(
          p, "fresh",
          json_object_new_boolean(feature_sample_fresh(&samples[i], now_ms())));
      json_build_array_add(a, p);
    }
#ifdef FEATURE_DEVICE_A1
    json_build_object_add(o, "average", average_body(samples, c->peer_count, now_ms()));
    json_object *members = json_object_new_array();
    for (unsigned i = 0; i < c->member_count; i++) {
      json_object *m = json_object_new_object();
      js(m, "mac", c->members[i].id);
      js(m, "ip", c->members[i].ip[0] ? c->members[i].ip : group_members[i].ip);
      json_build_object_add(
          m, "pending",
          json_object_new_boolean(group_members[i].pending != NULL));
      js(m, "transport",
         group_members[i].lan && !c->group_force_mqtt ? "lan" : "mqtt");
      number(m, "timeouts", group_members[i].failures);
      json_build_array_add(members, m);
    }
    json_build_object_add(o, "members", members);
#endif
    js(o, "linkage_status",
       !c->linkage.enabled ? "disabled"
       : data_valid        ? "active"
                           : "sensor_failed");
    unlock();
    json_build_object_add(o, "peers", a);
  } else
    o = config_encode(c, d);
  if (!strcmp(d, "binding")) {
#ifndef FEATURE_DEVICE_A1
    json_object_put(o);
    o = json_object_new_object();
#endif
    json_object *ids = json_object_new_array();
    for (unsigned i = 0; i < c->peer_count; i++) {
      char id[26];
#ifdef FEATURE_DEVICE_A1
      snprintf(id,sizeof(id),"%s:%s",LOCAL_ID,c->peers[i].id);
#else
      snprintf(id,sizeof(id),"%s:%s",c->peers[i].id,LOCAL_ID);
#endif
      json_build_array_add(ids,json_object_new_string(id));
    }
    json_build_object_add(o,"response_ids",ids);
  }
  free(c);
  return o;
}
int feature_set(const char *d, json_object *o, json_object **response) {
  int code = 200;
  if (!strcmp(d, "status")) {
    *response = json_object_new_object();
    number(*response, "code", 405);
    return 405;
  }
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return 500;
  feature_config_get(c);
#ifndef FEATURE_DEVICE_A1
  if (!strcmp(d,"linkage") || !strcmp(d,"light") || !strcmp(d,"probe")) {
    free(c); *response=json_object_new_object(); number(*response,"code",405);
    js(*response,"message","Linkage is read-only; configure rules on A1"); return 405;
  }
  if (!strcmp(d,"binding")) {
    json_object *ids=json_object_object_get(o,"response_ids");
    bool valid=ids && json_object_is_type(ids,json_type_array) && json_object_array_length(ids)<=8;
    feature_peer_t allowed[8]; memset(allowed,0,sizeof(allowed)); unsigned count=0;
    for (unsigned i=0; valid && i<(unsigned)json_object_array_length(ids); i++) {
      json_object *v=json_object_array_get_idx(ids,i);
      const char *id=json_object_get_string(v);
      char mac[13],sensor[13],raw[13];
      if (!json_object_is_type(v,json_type_string) || strlen(id)!=25 || id[12]!=':' ||
          json_object_get_string_len(v)!=25) { valid=false; break; }
      memcpy(raw,id,12);raw[12]=0;
      if (!feature_mac(raw,mac) || !feature_mac(id+13,sensor) || strcmp(sensor,LOCAL_ID) || !strcmp(mac,LOCAL_ID)) {valid=false;break;}
      bool duplicate=false;for(unsigned j=0;j<count;j++) duplicate|=!strcmp(allowed[j].id,mac);
      if (!duplicate) {strcpy(allowed[count].id,mac);allowed[count++].kind=FEATURE_A1;}
    }
    if (valid) {c->peer_count=count;memcpy(c->peers,allowed,sizeof(allowed));valid=install_config(c);}
    *response=json_object_new_object();number(*response,"code",valid?200:400);
    free(c);return valid?200:400;
  }
#endif
  json_object *v = json_object_object_get(o, "revision");
  if (v && integer(o, "revision") != c->revision) {
    code = 409;
    goto end;
  }
  if (!strcmp(d, "probe")) {
    char mac[13];
    if (!feature_mac(string(o, "mac"), mac)) {
      code = 400;
      goto end;
    }
    bool found = false;
    lock();
    for (unsigned i = 0; i < c->peer_count; i++)
      if (!strcmp(c->peers[i].id, mac)) {
        peers[i].recover_at = 0;
        peers[i].pending = false;
        peers[i].successes = 0;
        peers[i].lan = false;
        found = true;
      }
    unlock();
    if (!found)
      found = group_manual_probe(c, mac);
    if (!found)
      code = 404;
    goto end;
  }
  if (!config_decode(c, d, o)) {
    code = 400;
    goto end;
  }
#ifdef FEATURE_DEVICE_A1
  if (!strcmp(d, "linkage") && c->linkage.enabled && !data_bound()) {
    code = 409;
    goto end;
  }
#endif
  if (!strcmp(d, "binding")) {
    feature_config_t *old = malloc(sizeof(*old));
    if (!old) {
      code = 500;
      goto end;
    }
    feature_config_get(old);
    for (unsigned i = 0; i < old->peer_count; i++) {
      bool keep = false;
      for (unsigned j = 0; j < c->peer_count; j++)
        keep |= !strcmp(old->peers[i].id, c->peers[j].id);
      if (!keep && old->peers[i].confirmed) {
        json_object *r = envelope(old, i, "unbind", next_request());
        send_peer(old, i, r, false);
        send_peer(old, i, r, true);
        json_object_put(r);
      }
    }
    free(old);
    for (unsigned i = 0; i < c->peer_count; i++)
      if (!c->peers[i].confirmed)
        new_token(c->peers[i].token);
  }
  if (!install_config(c))
    code = 409;
  else
    group_broadcast_config(d, o);
end:
  *response = json_object_new_object();
  number(*response, "code", code);
  number(*response, "revision", c->revision);
  js(*response, "message",
     code == 200 ? "Saved"
     : code == 409
         ? "Revision conflict, binding required, or persistence unavailable"
         : "Invalid configuration");
  json_object *request = json_object_object_get(o, "request_id");
  if (request)
    json_build_object_add(*response, "request_id", json_object_get(request));
  free(c);
  return code;
}
void feature_manual_fan(bool on) {
  lock();
  automatic_valid = false;
  schedule_valid = false;
  if (on)
    configuration.scheduled_off = 0;
  unlock();
  {
    feature_config_t *c = malloc(sizeof(*c));
    if (c) {
      feature_config_get(c);
      if (c->scheduled_off || c->scheduled_valid) {
        c->scheduled_valid = 0;
        if (on)
          c->scheduled_off = 0;
        install_config(c);
      }
      free(c);
    }
  }
}
void feature_effective_fan(bool requested_on, unsigned requested_speed,
                           bool *on, unsigned *speed) {
  lock();
  *on = automatic_valid  ? automatic_on
        : schedule_valid ? scheduled_on
                         : requested_on;

  *speed = automatic_valid  ? automatic_speed
           : schedule_valid ? scheduled_speed
                            : requested_speed;
  if (sleep_state.fan_sleep && *speed > sleep_state.max_speed)
    *speed = sleep_state.max_speed;
  automatic_speed = *speed;
  effective_on = *on;
  unlock();
}
unsigned feature_light_cap(unsigned brightness) {
  lock();
  if (sleep_state.light_sleep && brightness > sleep_state.brightness)
    brightness = sleep_state.brightness;
  unlock();
  return brightness;
}
void feature_light_frame(uint32_t out[32], unsigned brightness, bool enabled,
                         uint32_t color) {
  lock();
  feature_light_t l = configuration.light;
  float values[4];
  memcpy(values, aggregate, sizeof(values));
  bool valid[4] = {data_valid, data_valid, data_valid, data_valid};
  unsigned speed = effective_on ? automatic_speed : 0;
  feature_sleep_state_t sleep = sleep_state;
  unlock();
  unsigned original_brightness = brightness;
  if (sleep.light_sleep && brightness > sleep.brightness)
    brightness = sleep.brightness;
  if (!enabled || !brightness) {
    memset(out, 0, 128);
    return;
  }
  if (l.effect == EFFECT_DATA) {
    feature_render(&l, values, valid, speed, brightness, true, now_ms(), out);
    return;
  }
  if (l.effect != EFFECT_DATA && l.follow_fan)
    brightness = brightness * speed / 100;
  if (l.effect != EFFECT_FAN) {
    unsigned b = brightness;
    if (l.effect == EFFECT_BREATHE)
      b = brightness * (1 + sin(now_ms() / 2000.0)) / 2;
    for (unsigned i = 0; i < 32; i++) {
      unsigned r = (color >> 8) & 255, g = (color >> 16) & 255,
               bl = color & 255;
      if (l.effect == EFFECT_RAINBOW) {
        unsigned p = (i * 768 / 32 + now_ms() / 30) % 768;
        r = p < 256 ? 255 - p : p > 512 ? p - 512 : 0;
        g = p < 256 ? p : p < 512 ? 511 - p : 0;
        bl = p < 256 ? 0 : p < 512 ? p - 256 : 767 - p;
      }
      out[i] = ((g * b / 255) << 16) | ((r * b / 255) << 8) | (bl * b / 255);
    }
    return;
  }
  if (sleep.light_sleep || l.follow_fan)
    for (unsigned i = 0; i < 32; i++) {
      unsigned g = (out[i] >> 16) & 255, r = (out[i] >> 8) & 255,
               b = out[i] & 255;
      out[i] =
          ((g * brightness / (original_brightness ? original_brightness : 1))
           << 16) |
          ((r * brightness / (original_brightness ? original_brightness : 1))
           << 8) |
          (b * brightness / (original_brightness ? original_brightness : 1));
    }
}
unsigned feature_mqtt_filters(char filters[12][FEATURE_TOPIC_MAX]) {
  mqtt_account_t a=mqtt_snapshot();unsigned n=0;
  feature_config_t *c=malloc(sizeof(*c));if(!c)return 0;feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
  mqtt_link_topic(&a,"a1",LOCAL_ID,"+","response",filters[n++],FEATURE_TOPIC_MAX);
  mqtt_link_topic(&a,"a1",LOCAL_ID,"+","request",filters[n++],FEATURE_TOPIC_MAX);
  mqtt_aux(&a,"address","+",filters[n++],FEATURE_TOPIC_MAX);
  if(c->peer_count || c->group[0]) {char root[FEATURE_TOPIC_MAX];root_for(c,0,root);mqtt_format(filters[n++],FEATURE_TOPIC_MAX,"%s/#",root);}
#else
  for(unsigned i=0;i<c->peer_count && i<8;i++)mqtt_link_topic(&a,"m1",LOCAL_ID,c->peers[i].id,"request",filters[n++],FEATURE_TOPIC_MAX);
#endif
  free(c);return n;
}
/* Reconnect only when the subscription set changes, not on every rule save.
 * Keep the 12-topic comparison off the nested MQTT callback stack. */
bool feature_mqtt_filters_current(char filters[12][FEATURE_TOPIC_MAX], unsigned count) {
  char (*next)[FEATURE_TOPIC_MAX] = calloc(12, FEATURE_TOPIC_MAX);
  if (!next) return false;
  unsigned n = feature_mqtt_filters(next);
  bool same = n == count;
  for (unsigned i = 0; same && i < n; ++i)
    same = !strcmp(filters[i], next[i]);
  free(next);
  return same;
}
static bool virtual_input(const char *t, const void *data, size_t n,
                          bool retained);
bool feature_mqtt_input(const char *t, const void *data, size_t n,
                        bool retained) {
  mqtt_account_t account = mqtt_snapshot();
  char address_prefix[192];
  mqtt_aux(&account,"address","",address_prefix,sizeof(address_prefix));
  size_t address_length = strlen(address_prefix);
  if (!strncmp(t,address_prefix,address_length)) {
#ifndef FEATURE_DEVICE_A1
    return false;
#endif
    json_object *o = parse(data, n);
    if (o) {
      char mac[13];
      const char *ip = string(o, "ip");
      if (feature_mac(t + address_length, mac) && strlen(ip) < 16 &&
          inet_addr(ip) != INADDR_NONE && inet_addr(ip) != 0) {
        lock();
        for (unsigned i = 0; i < configuration.peer_count; i++)
          if (!strcmp(configuration.peers[i].id, mac)) {
            strcpy(peers[i].ip, ip);
            if (!retained)
              peers[i].last_seen = now_ms();
          }
        for (unsigned i = 0; i < configuration.member_count; i++)
          if (!strcmp(configuration.members[i].id, mac)) {
#ifdef FEATURE_DEVICE_A1
            strcpy(group_members[i].ip, ip);
#endif
            if (!retained)
              member_seen[i] = now_ms();
          }
        unlock();
      }
      json_object_put(o);
    }
    return true;
  }
  char link_prefix[FEATURE_TOPIC_MAX];mqtt_leaf(&account,"link/",link_prefix,sizeof(link_prefix));
  if(!strncmp(t,link_prefix,strlen(link_prefix))) {
    const char *peer=t+strlen(link_prefix);char source[13];
    bool valid=strlen(peer)>=13;memcpy(source,peer,valid?12:0);source[12]=0;
    valid=valid && peer[12]=='/' && (!strcmp(peer+13,"request") || !strcmp(peer+13,"response"));
    json_object *o=valid?parse(data,n):NULL;
    if(o){if(!strcmp(string(o,"src"),source) && !strcmp(string(o,"dst"),LOCAL_ID))incoming(o,false,retained,NULL);json_object_put(o);}
    return true;
  }
  char original[FEATURE_TOPIC_MAX];
#ifdef FEATURE_DEVICE_A1
  mqtt_leaf(&account,"",original,sizeof(original));
#else
  mqtt_leaf(&account,"",original,sizeof(original));
#endif
  if (!strncmp(t, original, strlen(original))) {
    const char *leaf = t + strlen(original);
    const char *domains[] = {"binding",  "linkage", "light",
                             "schedule", "sleep",   "time"};
    for (unsigned d = 0; d < 6; d++) {
      char action[40], result[FEATURE_TOPIC_MAX];
      mqtt_format(action, sizeof(action), "%s/set", topic_domain(domains[d]));
      if (!strcmp(leaf, action)) {
        if (retained)
          return true;
        json_object *o = parse(data, n), *r = NULL;
        if (o) {
          feature_set(domains[d], o, &r);
          json_object_put(o);
          if (r) {
            mqtt_format(result, sizeof(result), "%s%s/result", original,
                     topic_domain(domains[d]));
            enqueue(result, r, false);
            json_object_put(r);
          }
        }
        return true;
      }
      mqtt_format(action, sizeof(action), "%s/get", topic_domain(domains[d]));
      if (!strcmp(leaf, action)) {
        if (retained)
          return true;
        json_object *o = feature_json(domains[d]);
        if (o) {
          mqtt_format(result, sizeof(result), "%s%s/state", original,
                   topic_domain(domains[d]));
          enqueue(result, o, true);
          json_object_put(o);
        }
        return true;
      }
    }
  }
  return virtual_input(t, data, n, retained);
}
static void virtual_service(int (*publish)(const char *, const char *, bool));
void feature_mqtt_session(const mqtt_account_t *account) {
  mico_rtos_lock_mutex(&namespace_lock);
  bool changed=feature_account.mode!=account->mode || strcmp(feature_account.key,account->key) ||
      strcmp(feature_account.model,account->model) || strcmp(feature_account.mac,account->mac);
  feature_account = *account;
  mico_rtos_unlock_mutex(&namespace_lock);
  lock();
  for (unsigned i=0; i<queued; ++i) free(queue[i].body);
  queued=queued_bytes=0; mqtt_online=false;
  /* A pending LAN RPC must not fall back into a different MQTT session. */
  for (unsigned i=0; i<8; ++i) {
    if (peers[i].rpc) json_object_put(peers[i].rpc);
    peers[i].rpc=NULL;
#ifdef FEATURE_DEVICE_A1
    if (group_members[i].pending) json_object_put(group_members[i].pending);
    group_members[i].pending=NULL;
#endif
  }
#ifdef FEATURE_DEVICE_A1
  mqtt_tick=0;
  if(changed) {
    published_root[0]=0; published_count=0;
    free(published_layout); published_layout=NULL;
  }
#else
  if(changed) for(unsigned i=0;i<8;i++) {
    peers[i].session_valid=false;memset(peers[i].session_token,0,16);
  }
#endif
  unlock();
}
static int (*active_publish)(const char *, const char *, bool);
static int checked_publish(const char *topic, const char *body, bool retain) {
  if (publish_error) return publish_error;
  publish_error=active_publish(topic,body,retain); return publish_error;
}
int feature_mqtt_service(int (*publish)(const char *, const char *, bool), bool connected) {
  if (!connected) { mqtt_online=false; return 0; }
  active_publish=publish; publish_error=0;
  virtual_service(checked_publish);
  for (unsigned i=0; i<4 && !publish_error; ++i) {
    char topic[FEATURE_TOPIC_MAX]; char *body=NULL; bool retained=false;
    lock();
    if (queued) {
      strcpy(topic,queue[0].topic); body=queue[0].body; retained=queue[0].retained;
      queued--; queued_bytes-=strlen(body)+1;
      memmove(queue,queue+1,queued*sizeof(queue[0]));
    }
    unlock();
    if (!body) break;
    if (scoped_topic(topic)) checked_publish(topic,body,retained);
    free(body);
  }
  return publish_error;
}
static void rpc_poll(const feature_config_t *c, unsigned i, uint32_t now) {
  json_object *expired = NULL;
  lock();
  if (peers[i].rpc &&
      (uint32_t)(now - peers[i].rpc_sent) >= c->peers[i].timeout_ms) {
    expired = peers[i].rpc;
    peers[i].rpc = NULL;
    peers[i].lan = false;
    peers[i].successes = 0;
    peers[i].recover_at = now + c->peers[i].recovery_seconds * 1000;
  }
  unlock();
  if (expired) {
    send_peer(c, i, expired, true);
    json_object_put(expired);
  }
}
static void feature_worker(mico_thread_arg_t arg) {
  UNUSED_PARAMETER(arg);
  char *wire = malloc(FEATURE_WIRE_MAX + 1);
  feature_config_t *c = malloc(sizeof(*c));
  if (!wire || !c) {
    free(wire);
    free(c);
    return;
  }
  udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp >= 0) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(FEATURE_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(udp, (struct sockaddr *)&a, sizeof(a)) < 0) {
      close(udp);
      udp = -1;
    }
  }
  for (;;) {
    uint32_t now = now_ms();
    feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
    a1_state_t state;
    a1_state_get(&state);
    a1_config_t basecfg;
    a1_config_get(&basecfg);
#else
    m1_state_t state;
    m1_state_get(&state);
    feature_sample_t local;
    memset(&local, 0, sizeof(local));
    local.value[0] = state.sample.temperature_deci_c / 10.0f;
    local.value[1] = state.sample.humidity_deci_pct / 10.0f;
    local.value[2] = state.sample.pm25_ug_m3;
    local.value[3] = state.sample.hcho_ug_m3 / 1000.0f;
    local.sequence = state.sample.sequence;
    local.boot = boot_id;
    local.received_ms = state.sample.updated_ms;
    local.valid = state.sample.received;
    feature_local_sample(&local);
#endif
    automation_poll(clock_ok);
    /* Local night windows apply on both products, independent of linkage. */
    mico_utc_time_ms_t local_utc = 0;
    if (clock_ok && mico_time_get_utc_time_ms(&local_utc) == kNoErr && local_utc / 1000 >= 1577836800) {
      uint64_t local_time = local_utc / 1000 + c->timezone_minutes * 60;
      unsigned local_day = (local_time / 86400 + 3) % 7;
      lock(); sleep_state = feature_sleep_at(c,local_day,(local_time / 60) % 1440); unlock();
    }
#ifdef FEATURE_DEVICE_A1
    if (state.mqtt_connected && (uint32_t)(now - announce_at) >= 5000) {
      announce_at = now;
      char topic[FEATURE_TOPIC_MAX];
      mqtt_account_t account = mqtt_snapshot();
      mqtt_aux(&account,"address",LOCAL_ID,topic,sizeof(topic));
      json_object *o = json_object_new_object();
      js(o, "mac", LOCAL_ID);
      js(o, "ip", state.ip);
      js(o, "group", c->group);
      number(o, "boot", boot_id);
#ifdef FEATURE_DEVICE_A1
      number(o, "kind", FEATURE_A1);
#else
      number(o, "kind", FEATURE_M1);
#endif
      enqueue(topic, o, true);
      enqueue(topic, o, false);
      json_object_put(o);
    }
    if (state.mqtt_connected && (uint32_t)(now - handshake_at) >= 5000) {
      handshake_at = now;
      for (unsigned step = 0; step < c->peer_count; step++) {
        unsigned i = (handshake_cursor + step) % c->peer_count;
        {
          json_object *o = envelope(c, i, "bind", next_request());
          send_peer(c, i, o, true);
          json_object_put(o);
        }
      }
      handshake_cursor++;
    }
    group_poll(c, now);
    for (unsigned i = 0; i < c->peer_count; i++) {
      lock();
      bool current = peer_current(c, i);
      unlock();
      if (!current)
        break;
      if (!c->peers[i].confirmed)
        continue;
      rpc_poll(c, i, now);
      if (state.mqtt_connected &&
          (uint32_t)(now - peers[i].mqtt_sent) >= 5000 &&
          (c->peers[i].force_mqtt || !peers[i].lan)) {
        json_object *o = envelope(c, i, "read", next_request());
        bool sent = send_peer(c, i, o, true);
        json_object_put(o);
        if (sent)
          peers[i].mqtt_sent = now;
      }
      if (c->peers[i].force_mqtt)
        continue;
      char ip[16];
      bool timeout, pending, lan;
      uint32_t recover;
      lock();
      strcpy(ip, c->peers[i].ip[0] ? c->peers[i].ip : peers[i].ip);
      timeout = peers[i].pending &&
                (uint32_t)(now - peers[i].sent) >= c->peers[i].timeout_ms;
      if (timeout) {
        peers[i].pending = false;
        peers[i].lan = false;
        peers[i].successes = 0;
        peers[i].recover_at = now + c->peers[i].recovery_seconds * 1000;
      }
      pending = peers[i].pending;
      lan = peers[i].lan;
      recover = peers[i].recover_at;
      unlock();
      if (timeout) {
        json_object *o = envelope(c, i, "read", next_request());
        send_peer(c, i, o, true);
        json_object_put(o);
      }
      if (!pending && ip[0] &&
          (lan ? (uint32_t)(now - peers[i].sent) >= 5000
               : (int32_t)(now - recover) >= 0 &&
                     (recover == 0 || c->peers[i].recovery))) {
        unsigned request = next_request();
        json_object *o = envelope(c, i, lan ? "read" : "probe", request);
        if (send_udp(ip, o)) {
          lock();
          peers[i].request = request;
          peers[i].sent = now;
          peers[i].pending = true;
          unlock();
        } else {
          lock();
          peers[i].lan = false;
          peers[i].successes = 0;
          peers[i].recover_at = now + c->peers[i].recovery_seconds * 1000;
          unlock();
          send_peer(c, i, o, true);
        }
        json_object_put(o);
      }
    }
#endif
#ifdef FEATURE_DEVICE_A1
    if ((uint32_t)(now - tick_at) >= 5000) {
      tick_at = now;
      feature_sample_t current[8];
      lock();
      memcpy(current, samples, sizeof(current));
      unlock();
      for (unsigned i = 0; i < c->peer_count; i++)
        if (!c->peers[i].confirmed)
          current[i].valid = false;
      float v[4];
      unsigned n = feature_aggregate(current, c->peer_count, now,
                                     c->linkage.statistic, v, NULL);
      bool on;
      unsigned speed;
      feature_effective_fan(basecfg.fan_on, basecfg.speed, &on, &speed);
      mico_utc_time_ms_t utc = 0;
      bool utc_valid =
          mico_time_get_utc_time_ms(&utc) == kNoErr && utc / 1000 >= 1577836800;
      if (clock_ok && utc_valid) {
        int64_t local = (int64_t)(utc / 1000) + c->timezone_minutes * 60;
        unsigned day = (local / 86400 + 3) % 7, minute = (local % 86400) / 60;
        lock();
        sleep_state = feature_sleep_at(c, day, minute);
        unlock();
      }
      lock();
      available = n;
      data_valid = n > 0;
      if (n) {
        memcpy(aggregate, v, sizeof(v));
        bool nexton;
        unsigned nextspeed;
        if (feature_control(&c->linkage, &pid, v, now, on, speed, &nexton,
                            &nextspeed)) {
          if (!on && sleep_state.fan_sleep && sleep_state.block_start)
            nexton = false;
          automatic_on = nexton;
          automatic_speed = nextspeed;
          automatic_valid = c->linkage.enabled;
        }
      } else
        memset(&pid, 0, sizeof(pid));
      unlock();
    }
#endif
    if (udp >= 0) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(udp, &fds);
      struct timeval tv = {0, 100000};
      if (select(udp + 1, &fds, NULL, NULL, &tv) > 0) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        int n = recvfrom(udp, wire, FEATURE_WIRE_MAX + 1, 0,
                         (struct sockaddr *)&from, &length);
        if (n > 0 && n <= FEATURE_WIRE_MAX) {
          json_object *o = parse(wire, n);
          if (o) {
            char ip[16];
            mqtt_format(ip, sizeof(ip), "%s", inet_ntoa(from.sin_addr));
            incoming(o, true, false, ip);
            json_object_put(o);
          }
        }
      }
    } else
      mico_thread_msleep(100);
    watchdog_beat(WATCHDOG_BINDING);
  }
}
void feature_clock_tick(void) {
  static uint32_t next = 0;
  uint32_t now = now_ms();
  if ((int32_t)(now - next) < 0)
    return;
  next = now + (clock_ok ? 3600000 : 60000);
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return;
  feature_config_get(c);
  struct hostent *host = gethostbyname(c->ntp_host);
  if (host && host->h_addr_list[0]) {
    struct in_addr address;
    memcpy(&address.s_addr, host->h_addr_list[0], 4);
    ntp_timestamp_t stamp;
    if (sntp_get_time(&address, &stamp) == kNoErr &&
        stamp.seconds >= 1577836800) {
      mico_utc_time_ms_t utc = (uint64_t)stamp.seconds * 1000;
      mico_time_set_utc_time_ms(&utc);
      feature_clock_valid();
    }
  }
  free(c);
}
OSStatus feature_start(void) {
  OSStatus e = mico_rtos_init_mutex(&namespace_lock);
  if (e != kNoErr) return e;
  e = mico_rtos_init_mutex(&feature_lock);
  if (e != kNoErr)
    return e;
#ifdef FEATURE_DEVICE_A1
  mqtt_account_init(&feature_account,MQTT_TOPIC_TMCP,"a1","",LOCAL_ID);
#else
  mqtt_account_init(&feature_account,MQTT_TOPIC_TMCP,"m1","",LOCAL_ID);
#endif
  feature_config_get(&configuration);
  schedule_valid = false;
  scheduled_on = !configuration.scheduled_off;
  scheduled_speed = configuration.scheduled_speed;
  if (MicoRandomNumberRead((uint8_t *)&boot_id, sizeof(boot_id)) != kNoErr)
    return kUnknownErr;
  return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "binding",
                                 feature_worker, FEATURE_WORKER_STACK_SIZE, 0);
}
static const char *domains[] = {"binding",  "linkage", "light",
                                "schedule", "sleep",   "time"};
static bool virtual_input(const char *t, const void *data, size_t n,
                          bool retained) {
#ifndef FEATURE_DEVICE_A1
  return false;
#endif
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return false;
  feature_config_get(c);
  char root[FEATURE_TOPIC_MAX];
  root_for(c, 0, root);
  const char *leaf = NULL;
#ifndef FEATURE_DEVICE_A1
  char selected_root[FEATURE_TOPIC_MAX];
  mqtt_format(selected_root, sizeof(selected_root), "%s", root);
#endif
#ifdef FEATURE_DEVICE_A1
  if (!strncmp(t, root, strlen(root)) && t[strlen(root)] == '/')
    leaf = t + strlen(root) + 1;
#else
  for (unsigned i = 0; i < c->peer_count; i++) {
    root_for(c, i, root);
    if (!strncmp(t, root, strlen(root)) && t[strlen(root)] == '/') {
      leaf = t + strlen(root) + 1;
      break;
    }
  }
#endif
  if (!leaf) {
    free(c);
    return false;
  }
  if (retained) {
    free(c);
    return true;
  }
#ifdef FEATURE_DEVICE_A1
  char member[20], member_mac[18];
  feature_colon_mac(LOCAL_ID, member_mac);
  mqtt_format(member, sizeof(member), "%s/", member_mac);
  if (!strncmp(leaf, member, strlen(member)))
    leaf += strlen(member);
  if(!strcmp(leaf,"command")){
    if(feature_is_coordinator()){
      json_object *q=service_parse(data,n),*reply=service_command(q);if(q)json_object_put(q);
      if(reply){char topic[FEATURE_TOPIC_MAX];mqtt_format(topic,sizeof(topic),"%s/result",root);enqueue(topic,reply,false);json_object_put(reply);}
    }
    free(c);return true;
  }
  for (unsigned d = 0; d < COUNT(domains); d++) {
    char get[40], set[40], topic[FEATURE_TOPIC_MAX];
    mqtt_format(get, sizeof(get), "%s/get", topic_domain(domains[d]));
    mqtt_format(set, sizeof(set), "%s/set", topic_domain(domains[d]));
    if (!strcmp(leaf, get)) {
      if (feature_is_coordinator()) {
        json_object *o = feature_json(domains[d]);
        mqtt_format(topic, sizeof(topic), "%s/%s/state", root,
                 topic_domain(domains[d]));
        if (o) {
          enqueue(topic, o, true);
          json_object_put(o);
        }
      }
      free(c);
      return true;
    }
    if (!strcmp(leaf, set)) {
      if (c->group[0] && !feature_is_coordinator()) {
        free(c);
        return true;
      }
      json_object *o = parse(data, n), *r = NULL;
      if (o) {
        int code = feature_set(domains[d], o, &r);
        if (code == 200) {
          json_object *state = feature_json(domains[d]);
          if (state) {
            js(state, "_source", LOCAL_ID);
            mqtt_format(topic, sizeof(topic), "%s/%s/state", root,
                     topic_domain(domains[d]));
            enqueue(topic, state, true);
            json_object_put(state);
          }
        }
        json_object_put(o);
        if (r && feature_is_coordinator()) {
          mqtt_format(topic, sizeof(topic), "%s/%s/result", root,
                   topic_domain(domains[d]));
          enqueue(topic, r, false);
        }
        if (r)
          json_object_put(r);
      }
      free(c);
      return true;
    }
  }
  if (c->group[0])
    for (unsigned d = 0; d < COUNT(domains); d++) {
      char state[40];
      mqtt_format(state, sizeof(state), "%s/state", topic_domain(domains[d]));
      if (!strcmp(leaf, state)) {
        json_object *o = parse(data, n);
        if (o && strcmp(string(o, "_source"), LOCAL_ID) &&
            feature_mac(string(o, "_source"), (char[13]){0})) {
          bool member = false;
          for (unsigned i = 0; i < c->member_count; i++)
            member |= !strcmp(c->members[i].id, string(o, "_source"));
          if (member) {
            json_object *local = config_encode(c, domains[d]);
            json_object_object_del(o, "revision");
            json_object_object_del(o, "_source");
            if (local) {
              json_object_object_del(local, "revision");
              const char *local_wire = json_object_to_json_string(local);
              const char *remote_wire = json_object_to_json_string(o);
              if (local_wire && remote_wire && strcmp(local_wire, remote_wire) &&
                  config_decode(c, domains[d], o))
                install_config(c);
              json_object_put(local);
            }
          }
        }
        if (o)
          json_object_put(o);
        free(c);
        return true;
      }
    }
  const char *kind = NULL, *key = NULL;
  if (!strcmp(leaf, "fan/set")) {
    kind = "fan";
    key = "on";
  } else if (!strcmp(leaf, "fan/percentage/set")) {
    kind = "fan";
    key = "speed";
  } else if (!strcmp(leaf, "light/set"))
    kind = "light";
  else if (!strcmp(leaf, "child_lock/set")) {
    kind = "child_lock";
    key = "enabled";
  } else if (!strcmp(leaf, "led_always_on/set")) {
    kind = "light";
    key = "always_on";
  }
  if (kind && n && n < FEATURE_WIRE_MAX) {
    json_object *o = NULL;
    if (!key)
      o = parse(data, n);
    else {
      char b[16];
      if (n < sizeof(b)) {
        memcpy(b, data, n);
        b[n] = 0;
        o = json_object_new_object();
        if (!strcmp(key, "speed")) {
          char *end;
          long value = strtol(b, &end, 10);
          if (end == b || *end || value < 0 || value > 100) {
            json_object_put(o);
            o = NULL;
          } else
            number(o, key, value);
        } else if (!strcmp(b, "ON") || !strcmp(b, "OFF"))
          json_build_object_add(o, key,
                                 json_object_new_boolean(!strcmp(b, "ON")));
        else {
          json_object_put(o);
          o = NULL;
        }
      }
    }
    if (o) {
      a1_control_local(kind, o);
      json_object_put(o);
    }
  }
#endif
  free(c);
  return true;
}
static void direct_leaf(int (*p)(const char *, const char *, bool),
                        const char *root, const char *leaf, const char *body,
                        bool retain) {
  if (!body) return;
  char topic[FEATURE_TOPIC_MAX];
  mqtt_format(topic, sizeof(topic), "%s/%s", root, leaf);
  p(topic, body, retain);
}
#ifdef FEATURE_DEVICE_A1
static void discovery_virtual(int (*publish)(const char *, const char *, bool),
                              const feature_config_t *c, const char *root,
                              bool remove) {
  const char *keys[] = {"fan",           "light",       "child_lock",
                        "led_always_on", "temperature", "humidity",
                        "pm25",          "formaldehyde"};
  const char *components[] = {"fan",    "light",  "switch", "switch",
                              "sensor", "sensor", "sensor", "sensor"};
  /* MQTT-owner-only scratch: publishing may dispatch callbacks on this stack. */
  static mqtt_account_t account;
  account = mqtt_snapshot();
  char owner[13]; strcpy(owner,LOCAL_ID);
  for (unsigned i=0; i<c->member_count; ++i)
    if (strcmp(c->members[i].id,owner)<0) strcpy(owner,c->members[i].id);
  static char safe[FEATURE_TOPIC_MAX];
  mqtt_format(safe, sizeof(safe), "%s", root);
  for (char *p = safe; *p; p++)
    if (*p == ':')
      *p = '_';
  if (account.mode) {
    const char *b = strrchr(root, '/');
    b = b ? b + 1 : root;
    if (!mqtt_hex(safe,sizeof(safe),b)) return;
  }
  unsigned sets = c->peer_count > 1 ? c->peer_count + 1 : 1;
  for (unsigned set = 0; set < sets; set++)
    for (unsigned i = 0; i < 8; i++) {
      if (set && i < 4)
        continue;
      static char uid[FEATURE_TOPIC_MAX], path[FEATURE_TOPIC_MAX], sensor[FEATURE_TOPIC_MAX];
      const char *sid = set ? c->peers[set - 1].id : "average";
      mqtt_format(uid, sizeof(uid), "bound_%s_%s_%s", safe, i < 4 ? "device" : sid,
               keys[i]);
      mqtt_format(path, sizeof(path), "homeassistant/%s/%s/config", components[i],
               uid);
      if (account.mode) {
        char entity[240];
        if (!mqtt_format(entity,sizeof(entity),"bound_%s_%s_%s",safe,i<4?"device":sid,keys[i])) return;
        strcpy(account.mac,owner);
        if (!mqtt_identity(&account,entity,uid,sizeof(uid)) || !mqtt_ha(&account,components[i],entity,path,sizeof(path))) return;
      }
      if (remove) {
        publish(path, "", true);
        continue;
      }
      json_object *o = json_object_new_object(),
                  *device = json_object_new_object(),
                  *ids = json_object_new_array();
      if (!o || !device || !ids) {
        if (o)
          json_object_put(o);
        if (device)
          json_object_put(device);
        if (ids)
          json_object_put(ids);
        return;
      }
      char device_id[192];
      if (account.mode) mqtt_format(device_id,sizeof(device_id),"v2_%s_%s_%s",account.key,c->group[0]?"group":owner,safe);
      else mqtt_format(device_id,sizeof(device_id),"%s",root);
      json_build_array_add(ids, json_object_new_string(device_id));
      json_build_object_add(device, "identifiers", ids);
      js(device, "name", c->group[0] ? c->group : root);
      js(device, "manufacturer", "Phicomm");
      json_build_object_add(o, "device", device);
      js(o, "unique_id", uid);
      json_build_object_add(o,"retain",json_object_new_boolean(false));
      js(o, "name", keys[i]);
      static char topic[FEATURE_TOPIC_MAX];
      if (i < 4) {
        mqtt_format(topic, sizeof(topic), "%s/command", root);
        js(o, "command_topic", topic);
        mqtt_format(topic, sizeof(topic), "%s/%s/state", root, keys[i]);
        js(o, "state_topic", topic);
        if (i == 0) {
          mqtt_format(topic, sizeof(topic), "%s/command", root);
          js(o, "percentage_command_topic", topic);
          js(o,"command_template","{\"action\":\"fan.set\",\"params\":{\"on\":{{ 'true' if value == 'ON' else 'false' }}}}");
          js(o,"percentage_command_template","{\"action\":\"fan.set\",\"params\":{\"speed\":{{ value | int }}}}");
          mqtt_format(topic, sizeof(topic), "%s/fan/percentage/state", root);
          js(o, "percentage_state_topic", topic);
          number(o, "speed_range_min", 1);
          number(o, "speed_range_max", 100);
        }
        if (i == 1) {
            js(o,"schema","template");
            js(o,"command_on_template","{\"action\":\"light.set\",\"params\":{\"enabled\":true{% if brightness is defined %},\"brightness\":{{ ((brightness | int) * 100 / 255) | round | int }}{% endif %}{% if red is defined %},\"color\":{\"r\":{{ red }},\"g\":{{ green }},\"b\":{{ blue }}}{% endif %}}}");
            js(o,"command_off_template","{\"action\":\"light.set\",\"params\":{\"enabled\":false}}");
            js(o,"state_template","{{ value_json.state | lower }}");
            js(o,"brightness_template","{{ value_json.brightness }}");
            js(o,"red_template","{{ value_json.color.r }}");
            js(o,"green_template","{{ value_json.color.g }}");
            js(o,"blue_template","{{ value_json.color.b }}");
        } else if(i==0) { js(o,"payload_on","ON");js(o,"payload_off","OFF"); }
        else {
            const char *on=i==2?"{\"action\":\"child_lock.set\",\"params\":{\"enabled\":true}}":"{\"action\":\"light.set\",\"params\":{\"always_on\":true}}";
            const char *off=i==2?"{\"action\":\"child_lock.set\",\"params\":{\"enabled\":false}}":"{\"action\":\"light.set\",\"params\":{\"always_on\":false}}";
            js(o,"payload_on",on);js(o,"payload_off",off);js(o,"state_on","ON");js(o,"state_off","OFF");
        }
            } else {
        if (c->peer_count == 1)
          mqtt_format(sensor, sizeof(sensor), "%s/sensor", root);
        else if (set) {
          char mac[18];
          feature_colon_mac(sid, mac);
          mqtt_format(sensor, sizeof(sensor), "%s/%s/sensor", root, mac);
        } else
          mqtt_format(sensor, sizeof(sensor), "%s/average", root);
        js(o, "state_topic", sensor);
        mqtt_format(topic, sizeof(topic), "{{ value_json.%s }}", keys[i]);
        js(o, "value_template", topic);
        number(o, "expire_after", 15);
        js(o, "state_class", "measurement");
        js(o, "unit_of_measurement",
           i == 4   ? "°C"
           : i == 5 ? "%"
           : i == 6 ? "µg/m³"
                    : "mg/m³");
        if (i != 7)
          js(o, "device_class", keys[i]);
      }
      mqtt_leaf(&account,"state",topic,sizeof(topic));js(o,"json_attributes_topic",topic);
      js(o,"json_attributes_template","{{ {\"ip\": value_json.ip, \"firmware\": value_json.version} | tojson }}");
      if(i==2 || i==3)js(o,"entity_category","config");
      json_object *availability = json_object_new_array();
      for (unsigned member = 0; member <= c->member_count; member++) {
        const char *id = member ? c->members[member - 1].id : LOCAL_ID;
        if (member && !strcmp(id, LOCAL_ID))
          continue;
        json_object *node = json_object_new_object();
        static mqtt_account_t member_account;
        /* Reuse the already encoded owner namespace without decoding credentials. */
        member_account=account;
        mqtt_format(member_account.base,sizeof(member_account.base),"phicomm/v2/%s/a1/%s",account.key,id);
        mqtt_leaf(&member_account,"availability",topic,sizeof(topic));
        js(node, "topic", topic);
        json_build_array_add(availability, node);
      }
      json_build_object_add(o, "availability", availability);
      js(o, "availability_mode", "any");
      const char *wire = json_object_to_json_string(o);
      if (wire) publish(path, wire, true);
      json_object_put(o);
    }
}
#endif
static void virtual_service(int (*publish)(const char *, const char *, bool)) {
  uint32_t now = now_ms();
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return;
  feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
  bool bound = feature_bound() || c->group[0];
  char root[FEATURE_TOPIC_MAX];
  root_for(c, 0, root);
  bool coordinator = feature_is_coordinator();
  if ((!mqtt_online || mqtt_revision != c->revision ||
       (!was_coordinator && coordinator)) &&
      coordinator) {
    bool layout_changed =
        strcmp(published_root, root) || published_count != c->peer_count ||
        (published_layout && (published_layout->member_count != c->member_count || memcmp(published_layout->members,c->members,sizeof(c->members))));
    for (unsigned i = 0; i < c->peer_count && !layout_changed; i++)
      layout_changed = strcmp(published_ids[i], c->peers[i].id) != 0;
    if (published_root[0] && (layout_changed || !bound)) {
      if (published_layout) discovery_virtual(publish,published_layout,published_root,true);
    }
    if (bound) {
      discovery_virtual(publish, c, root, false);
      if(!published_layout) published_layout=malloc(sizeof(*published_layout));
      if(published_layout) *published_layout=*c;
      strcpy(published_root, root);
      published_count = c->peer_count;
      for (unsigned i = 0; i < c->peer_count; i++)
        strcpy(published_ids[i], c->peers[i].id);
      for (unsigned d = 0; d < COUNT(domains); d++) {
        json_object *o = config_encode(c, domains[d]);
        if (o) {
          js(o, "_source", LOCAL_ID);
          char leaf[40];
          mqtt_format(leaf, sizeof(leaf), "%s/state", topic_domain(domains[d]));
          direct_leaf(publish, root, leaf, json_object_to_json_string(o), true);
          json_object_put(o);
        }
      }
    } else
      published_root[0] = 0;
  }
  if (bound && (uint32_t)(now - mqtt_tick) >= 5000) {
    mqtt_tick = now;
    bool on;
    unsigned speed;
    /* These snapshots are only used by the MQTT owner. */
    static a1_config_t cfg;
    a1_config_get(&cfg);
    static a1_state_t state;
    a1_state_get(&state);
    unsigned light_brightness = cfg.brightness;
    lock();
    if (sleep_state.light_sleep && light_brightness > sleep_state.brightness)
      light_brightness = sleep_state.brightness;
    unlock();
    feature_effective_fan(cfg.fan_on, cfg.speed, &on, &speed);
    char prefix[FEATURE_TOPIC_MAX];
    bool leader = feature_is_coordinator();
    for (unsigned pass = 0; pass < (c->group[0] ? 2u : 1u); pass++) {
      if (!pass && !leader)
        continue;
      if (pass) {
        char mac[18];
        feature_colon_mac(LOCAL_ID, mac);
        mqtt_format(prefix, sizeof(prefix), "%s/%s", root, mac);
      } else
        strcpy(prefix, root);
      direct_leaf(publish, prefix, "fan/state", on ? "ON" : "OFF", true);
      char b[192];
      mqtt_format(b, sizeof(b), "%u", on ? speed : 0);
      direct_leaf(publish, prefix, "fan/percentage/state", b, true);
      mqtt_format(b, sizeof(b),
               "{\"state\":\"%s\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%"
               "u,\"b\":%u}}",
               state.led_visible ? "ON" : "OFF", light_brightness, cfg.red,
               cfg.green, cfg.blue);
      direct_leaf(publish, prefix, "light/state", b, true);
      direct_leaf(publish, prefix, "child_lock/state",
                  cfg.child_lock ? "ON" : "OFF", true);
      direct_leaf(publish, prefix, "led_always_on/state",
                  cfg.led_always_on ? "ON" : "OFF", true);
      direct_leaf(publish, prefix, "availability", "online", true);
    }
    if (leader) {
      static feature_sample_t current[8];
      lock();
      memcpy(current, samples, sizeof(current));
      unlock();
      json_object *average = average_body(current, c->peer_count, now);
      direct_leaf(publish, root, "average", json_object_to_json_string(average), false);
      json_object_put(average);
      for (unsigned i = 0; i < c->peer_count; i++)
        if (feature_sample_fresh(&current[i], now)) {
          json_object *o = sample_body(&current[i], now);
          char leaf[64];
          if (c->peer_count == 1)
            strcpy(leaf, "sensor");
          else {
            char mac[18];
            feature_colon_mac(c->peers[i].id, mac);
            mqtt_format(leaf, sizeof(leaf), "%s/sensor", mac);
          }
          direct_leaf(publish, root, leaf, json_object_to_json_string(o),
                      false);
          json_object_put(o);
        }
    }
  }
#endif
  mqtt_revision = c->revision;
  mqtt_online = true;
#ifdef FEATURE_DEVICE_A1
  was_coordinator = coordinator;
#endif
  free(c);
}

int feature_mqtt_cleanup(int (*publish)(const char *, const char *, bool)) {
  active_publish=publish; publish_error=0;
#ifdef FEATURE_DEVICE_A1
  if (published_root[0]) {
    if(!published_layout) return -1;
    discovery_virtual(checked_publish,published_layout,published_root,true);
  }
#endif
  return publish_error;
}
