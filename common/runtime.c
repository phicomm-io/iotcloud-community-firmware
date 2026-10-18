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
static void reset_controller(bool preserve_gate);
static uint32_t boot_id, serial, announce_at, handshake_at;
static unsigned handshake_cursor;
#ifdef FEATURE_DEVICE_A1
static uint32_t tick_at;
#endif
static bool automatic_on, automatic_valid, scheduled_on, schedule_valid,
    clock_ok;
static unsigned automatic_speed, scheduled_speed;
static int automatic_night = -1;
static bool effective_on;
static unsigned effective_speed;
static bool automatic_immediate = true;

static feature_sleep_state_t sleep_state;
static float aggregate[4], aggregate_co2;
static unsigned available_co2;
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
static unsigned published_sensors;
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
  bool preserve_gate = configuration.linkage.enabled && c->linkage.enabled &&
                       configuration.linkage.hysteresis_enabled && c->linkage.hysteresis_enabled;
  configuration = *c;
  automatic_immediate = true;
  automatic_valid = false;
  reset_controller(preserve_gate);
  unlock();
  free(next);
  return true;
}
static bool scd40_aux_fresh(const feature_sample_t *s, uint32_t now) {
  return s->scd40_aux_supported && s->scd40_aux_valid && feature_co2_fresh(s, now) &&
    isfinite(s->scd40_temperature) && s->scd40_temperature >= -45 && s->scd40_temperature <= 130 &&
    isfinite(s->scd40_humidity) && s->scd40_humidity >= 0 && s->scd40_humidity <= 100;
}
static void co2_body(json_object *o, const feature_sample_t *s, uint32_t now) {
  json_build_object_add(o,"co2_supported",json_object_new_boolean(s->co2_supported));
  json_build_object_add(o,"co2_valid",json_object_new_boolean(feature_co2_fresh(s,now)));
  json_build_object_add(o,"scd40_aux_supported",json_object_new_boolean(s->scd40_aux_supported));
  json_build_object_add(o,"scd40_aux_valid",json_object_new_boolean(scd40_aux_fresh(s,now)));
  json_object_object_add(o,"scd40_temperature",scd40_aux_fresh(s,now)?json_object_new_double(s->scd40_temperature):NULL);
  json_object_object_add(o,"scd40_humidity",scd40_aux_fresh(s,now)?json_object_new_double(s->scd40_humidity):NULL);
  number(o,"co2_age_ms",s->co2_age_ms+(uint32_t)(now-s->co2_received_ms));
  if(feature_co2_fresh(s,now)) json_build_object_add(o,"co2",json_object_new_double(s->co2));
  else json_object_object_add(o,"co2",NULL);
}
static json_object *sample_body(const feature_sample_t *s, uint32_t now) {
  json_object *o = json_object_new_object();
  if (!o)
    return NULL;
  co2_body(o,s,now);
  number(o, "sequence", s->sequence);
  number(o, "boot", s->boot);
  number(o, "age_ms", s->age_ms + (uint32_t)(now - s->received_ms));
  json_build_object_add(o, "valid",
                         json_object_new_boolean(feature_sample_fresh(s, now)));
  const char *keys[] = {"temperature", "humidity", "pm25", "formaldehyde"};
  for (unsigned i = 0; i < 4; i++)
    json_object_object_add(o, keys[i], feature_sample_fresh(s,now)?json_object_new_double(s->value[i]):NULL);
  return o;
}
#ifdef FEATURE_DEVICE_A1
/* Caller supplies a stable snapshot. Missing/stale data is never a zero reading. */
static json_object *average_body(const feature_sample_t *current, unsigned count, uint32_t now) {
  float mean[4];
  unsigned n = feature_aggregate(current, count, now, STAT_MEAN, mean, NULL);
  json_object *o = json_object_new_object();
  number(o, "sample_count", n);
  float co2=0; unsigned nc=feature_co2_aggregate(current,count,now,STAT_MEAN,&co2);
  bool supported=false; for(unsigned i=0;i<count && i<8;i++) supported |= current[i].co2_supported;
  json_build_object_add(o,"co2_supported",json_object_new_boolean(supported));
  json_build_object_add(o,"co2_valid",json_object_new_boolean(nc != 0));
  number(o,"co2_sample_count",nc);
  if(nc) json_build_object_add(o,"co2",json_object_new_double(co2));
  else json_object_object_add(o,"co2",NULL);
  float st=0, sh=0; unsigned ns=0; bool aux_supported=false;
  for(unsigned i=0;i<count && i<8;i++) {
    aux_supported |= current[i].scd40_aux_supported;
    if(scd40_aux_fresh(&current[i],now)) { st+=current[i].scd40_temperature; sh+=current[i].scd40_humidity; ns++; }
  }
  json_build_object_add(o,"scd40_aux_supported",json_object_new_boolean(aux_supported));
  json_build_object_add(o,"scd40_aux_valid",json_object_new_boolean(ns != 0));
  number(o,"scd40_sample_count",ns);
  json_object_object_add(o,"scd40_temperature",ns?json_object_new_double(st/ns):NULL);
  json_object_object_add(o,"scd40_humidity",ns?json_object_new_double(sh/ns):NULL);
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
    json_object *co2=json_object_object_get(b,"co2"), *age=json_object_object_get(b,"co2_age_ms");
    s.co2_supported=integer(b,"co2_supported");
    s.co2_valid=s.co2_supported && integer(b,"co2_valid") && co2 && age &&
      (json_object_is_type(co2,json_type_double)||json_object_is_type(co2,json_type_int)) &&
      json_object_is_type(age,json_type_int) && json_object_get_int64(age)>=0 && json_object_get_int64(age)<20000;
    s.co2=json_object_get_double(co2); s.co2_age_ms=integer(b,"co2_age_ms"); s.co2_received_ms=now_ms();
    json_object *st=json_object_object_get(b,"scd40_temperature"), *sh=json_object_object_get(b,"scd40_humidity");
    s.scd40_aux_supported=integer(b,"scd40_aux_supported");
    s.scd40_aux_valid=integer(b,"scd40_aux_valid") && st && sh &&
      (json_object_is_type(st,json_type_double)||json_object_is_type(st,json_type_int)) &&
      (json_object_is_type(sh,json_type_double)||json_object_is_type(sh,json_type_int));
    s.scd40_temperature=json_object_get_double(st); s.scd40_humidity=json_object_get_double(sh);
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
    number(o, "co2_available", available_co2);
    json_build_object_add(o, "data_valid",
                           json_object_new_boolean(data_valid));
    json_build_object_add(o, "clock_valid", json_object_new_boolean(clock_ok));
    json_build_object_add(o, "automatic_on",
                           json_object_new_boolean(effective_on));
    number(o, "automatic_speed", automatic_speed);
    json_object *a = json_object_new_array();
    for (unsigned i = 0; i < c->peer_count; i++) {
      json_object *p = json_object_new_object();
      js(p, "mac", c->peers[i].id);
      json_build_object_add(p,"co2_supported",json_object_new_boolean(samples[i].co2_supported));
      json_build_object_add(p,"co2_valid",json_object_new_boolean(feature_co2_fresh(&samples[i],now_ms())));
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
/* Reinitialize PID math without erasing the hysteresis latch or dwell time
 * when a limit, night profile, or unrelated setting changes. */
static void reset_controller(bool preserve_gate) {
  feature_pid_t old = pid;
  memset(&pid, 0, sizeof(pid));
  if (preserve_gate) {
    pid.gate_running = old.gate_running;
    pid.switched = old.switched;
    pid.switch_ms = old.switch_ms;
    pid.condition = old.condition;
    pid.condition_since = old.condition_since;
  }
}
void feature_manual_fan(bool on) {
  lock();
  automatic_valid = false;
  automatic_immediate = true;
  reset_controller(on && configuration.linkage.enabled && configuration.linkage.hysteresis_enabled);
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
  *on = requested_on;
  *speed = requested_on ? requested_speed : 0;
#ifdef FEATURE_DEVICE_A1
  if (configuration.linkage.enabled && requested_on) {
    feature_linkage_t policy = configuration.linkage;
    unsigned fallback = configuration.automatic.fallback_speed;
    if (automatic_night >= 0 && (unsigned)automatic_night < configuration.sleep_count) {
      policy.min_speed = configuration.automatic.night[automatic_night].min_speed;
      policy.max_speed = configuration.sleeps[automatic_night].max_speed;
      fallback = configuration.automatic.night[automatic_night].fallback_speed;
    }
    bool selected_valid = data_valid && (!(policy.metrics & 3) || available > 0) &&
                          (!(policy.metrics & 4) || available_co2 > 0);
    if (!automatic_valid) {
      if (selected_valid) {
        bool ignored;
        if (automatic_immediate && !policy.hysteresis_enabled) policy.debounce_seconds=policy.min_switch_seconds=0;
        feature_control_co2(&policy, &configuration.co2, &pid, aggregate, aggregate_co2, now_ms(), true,
                        automatic_speed, &ignored, &automatic_speed);
      } else automatic_speed = (unsigned)clip(fallback, policy.min_speed, policy.max_speed);
      automatic_valid = true;
      automatic_immediate = false;
    }
    *speed = policy.hysteresis_enabled && selected_valid && !pid.gate_running ? 0 :
             (unsigned)clip(automatic_speed,policy.min_speed,policy.max_speed);
  } else
#endif
  if (requested_on && sleep_state.fan_sleep && *speed > sleep_state.max_speed)
    *speed = sleep_state.max_speed;
  if (!requested_on) {
    *speed = 0;
    reset_controller(false);
    automatic_valid = false;
    automatic_speed = 0;
  }
  effective_speed = *speed;
  automatic_on = *on;
  effective_on = *on;
  unlock();
}
bool feature_fan_auto(void) {
  lock(); bool automatic = configuration.linkage.enabled; unlock(); return automatic;
}
unsigned feature_fan_limit(void) {
  lock(); unsigned value = automatic_night >= 0 && (unsigned)automatic_night < configuration.sleep_count
    ? configuration.sleeps[automatic_night].max_speed : configuration.linkage.max_speed;
  unlock(); return value;
}
int feature_fan_command(json_object *o) {
  feature_config_t *c = malloc(sizeof(*c)); if (!c) return 0;
  feature_config_get(c);
  bool automatic = c->linkage.enabled;
  json_object *mode = json_object_object_get(o,"mode");
  if (mode) {
    if (!json_object_is_type(mode,json_type_string) || strlen(json_object_get_string(mode)) != (size_t)json_object_get_string_len(mode)) {free(c);return 0;}
    const char *v=json_object_get_string(mode);
    if (!strcmp(v,"auto")) automatic=true;
    else if (!strcmp(v,"manual")) automatic=false;
    else {free(c);return 0;}
  }
  bool changed = automatic != (bool)c->linkage.enabled;
  c->linkage.enabled = automatic;
  json_object *speed=json_object_object_get(o,"speed");
  if (automatic && speed && json_object_get_int(speed)>0) {
    unsigned maximum=json_object_get_int(speed);
    lock(); int night=automatic_night; unlock();
    if (night>=0 && (unsigned)night<c->sleep_count) {
      changed |= c->sleeps[night].max_speed != maximum;
      c->sleeps[night].max_speed=maximum;
      if(c->automatic.night[night].min_speed>maximum) c->automatic.night[night].min_speed=maximum;
    } else {
      changed |= c->linkage.max_speed != maximum;
      c->linkage.max_speed=maximum;
      if(c->linkage.min_speed>maximum) c->linkage.min_speed=maximum;
    }
  }
  bool ok=!changed || install_config(c);
  free(c);return ok?(automatic?2:1):0;
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
  bool valid[4] = {available > 0, available > 0, available > 0, available > 0};
  unsigned speed = effective_on ? effective_speed : 0;
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
#ifdef PHICOMM_DEVICE_OTA
static bool worker_running, worker_stop, worker_exiting, worker_ready;
bool feature_running(void){lock();bool yes=worker_running;unlock();return yes;}
#endif
static void feature_worker(mico_thread_arg_t arg) {
  UNUSED_PARAMETER(arg);
  char *wire = malloc(FEATURE_WIRE_MAX + 1);
  feature_config_t *c = malloc(sizeof(*c));
  if (!wire || !c) {
    free(wire);
    free(c);
#ifdef PHICOMM_DEVICE_OTA
    watchdog_binding_enable(false);lock();worker_running=false;worker_ready=false;unlock();
#endif
#ifdef PHICOMM_DEVICE_OTA
    mico_rtos_delete_thread(NULL);
#endif
    return;
  }
#ifdef PHICOMM_DEVICE_OTA
  lock();worker_ready=true;unlock();
#endif
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
#ifdef PHICOMM_DEVICE_OTA
    lock();bool stop=worker_stop;if(stop)worker_exiting=true;unlock();if(stop)break;
#endif
    uint32_t now = now_ms();
    feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
    a1_state_t state;
    a1_state_get(&state);

#else
    m1_state_t state;
    m1_state_get(&state);
    feature_sample_t local;
    memset(&local, 0, sizeof(local));
    local.value[0] = state.sample.temperature_deci_c / 10.0f;
    local.value[1] = state.sample.humidity_deci_pct / 10.0f;
    local.value[2] = state.sample.pm25_ug_m3;
    local.value[3] = state.sample.hcho_ug_m3 / 1000.0f;
    local.co2_supported=state.scd40.ready || state.scd40.received;
    local.co2_valid=state.scd40.ready && state.scd40.received;
    local.scd40_aux_supported=local.co2_supported;
    local.scd40_aux_valid=local.co2_valid;
    local.scd40_temperature=state.scd40.temperature_milli_c/1000.0f;
    local.scd40_humidity=state.scd40.humidity_milli_pct/1000.0f;
    local.co2=state.scd40.co2_ppm; local.co2_received_ms=state.scd40.updated_ms;
    static uint32_t wire_sequence, uart_sequence, co2_sequence;
    static bool co2_ready;
    if(!wire_sequence || uart_sequence!=state.sample.sequence || co2_sequence!=state.scd40.samples || co2_ready!=state.scd40.ready) {
      ++wire_sequence; uart_sequence=state.sample.sequence; co2_sequence=state.scd40.samples; co2_ready=state.scd40.ready;
    }
    local.sequence = wire_sequence;
    local.boot = boot_id;
    local.received_ms = state.sample.updated_ms;
    local.valid = state.sample.received;
    feature_local_sample(&local);
#endif
    automation_poll(clock_ok);
    /* Local night windows apply on both products, independent of linkage. */
    mico_utc_time_ms_t local_utc = 0;
    if (clock_ok && mico_time_get_utc_time_ms(&local_utc) == kNoErr && local_utc / 1000 >= 1577836800) {
      int64_t local_time = (int64_t)(local_utc / 1000) + c->timezone_minutes * 60;
      unsigned local_day = (local_time / 86400 + 3) % 7;
      unsigned minute = (local_time / 60) % 1440;
      lock(); sleep_state = feature_sleep_at(c,local_day,minute);
      int selected = -1;
      for (unsigned i=0;i<c->sleep_count;i++) {
        const feature_sleep_t *w = &c->sleeps[i];
        bool active = w->start < w->end ? ((w->days & (1u<<local_day)) && minute>=w->start && minute<w->end)
          : ((w->days & (1u<<local_day)) && minute>=w->start) || ((w->days & (1u<<((local_day+6)%7))) && minute<w->end);
        if (w->fan && active) selected = i;
      }
      if (automatic_night != selected) { automatic_night=selected; automatic_immediate=true; automatic_valid=false; reset_controller(configuration.linkage.hysteresis_enabled); }
      unlock();
    } else {
      lock(); sleep_state = (feature_sleep_state_t){false,false,false,100,255};
      if (automatic_night != -1) { automatic_night=-1; automatic_immediate=true; automatic_valid=false; reset_controller(configuration.linkage.hysteresis_enabled); }
      unlock();
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
        if (!c->peers[i].confirmed) {
          current[i].valid = false; current[i].co2_valid = false;
        }
      float v[4] = {0}, co2=0;
      unsigned nc=feature_co2_aggregate(current,c->peer_count,now,c->linkage.statistic,&co2);
      unsigned n = feature_aggregate(current, c->peer_count, now,
                                     c->linkage.statistic, v, NULL);
      lock();
      available = n;
      available_co2=nc; aggregate_co2=co2;
      data_valid = (!(c->linkage.metrics & 3) || n > 0) && (!(c->linkage.metrics & 4) || nc > 0);
      memcpy(aggregate,v,sizeof(v));
      if (!data_valid) { reset_controller(configuration.linkage.hysteresis_enabled); pid.condition=0; }
      automatic_valid = false;
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
#ifdef PHICOMM_DEVICE_OTA
  if(udp>=0){close(udp);udp=-1;}
  free(wire);free(c);
  lock();
  for(unsigned i=0;i<queued;i++)free(queue[i].body);
  queued=queued_bytes=0;
  for(unsigned i=0;i<8;i++){
    if(peers[i].rpc)json_object_put(peers[i].rpc);peers[i].rpc=NULL;
#ifdef FEATURE_DEVICE_A1
    if(group_members[i].pending)json_object_put(group_members[i].pending);group_members[i].pending=NULL;
#endif
  }
#ifdef FEATURE_DEVICE_A1
  free(published_layout);published_layout=NULL;published_root[0]=0;published_count=0;
#endif
  watchdog_binding_enable(false);
  worker_running=worker_ready=worker_exiting=false;unlock();
  mico_rtos_delete_thread(NULL);
#endif
}
#ifdef PHICOMM_DEVICE_OTA
bool feature_pause(unsigned timeout_ms){
  lock();worker_stop=true;unlock();
  uint32_t start=now_ms();
  while(feature_running() && (uint32_t)(now_ms()-start)<timeout_ms)mico_thread_msleep(20);
  if(feature_running()){lock();if(!worker_exiting)worker_stop=false;unlock();return false;}
  /* Let the RTOS finish deleting/reclaiming the old worker before recreating. */
  mico_thread_msleep(20);return true;
}
OSStatus feature_resume(void){
  /* Once exit was acknowledged it cannot be cancelled. Wait for cleanup. */
  for(;;){lock();bool exiting=worker_exiting;unlock();if(!exiting)break;mico_thread_msleep(20);}
  mico_thread_msleep(20);
  lock();if(worker_running){bool ready=worker_ready;worker_stop=false;unlock();return ready?kNoErr:kNoMemoryErr;}
  worker_stop=worker_ready=false;worker_running=true;unlock();watchdog_binding_enable(true);
  OSStatus e=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"binding",feature_worker,FEATURE_WORKER_STACK_SIZE,0);
  if(e!=kNoErr){watchdog_binding_enable(false);lock();worker_running=false;unlock();return e;}
  uint32_t start=now_ms();
  for(;;){lock();bool ready=worker_ready,running=worker_running;unlock();if(ready)return kNoErr;if(!running||(uint32_t)(now_ms()-start)>5000)return kNoMemoryErr;mico_thread_msleep(20);}
}
#endif
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
#ifdef PHICOMM_DEVICE_OTA
  return feature_resume();
#else
  return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "binding",
                                 feature_worker, FEATURE_WORKER_STACK_SIZE, 0);
#endif
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
/* Shared by virtual controls and per-A1 filter entities. Keep their parent
 * identity identical while preserving each physical filter's own state/control. */
static bool virtual_identity(const feature_config_t *c, const char *root,
                             const mqtt_account_t *account, char owner[13],
                             char *safe, size_t safe_size, char *id, size_t id_size) {
  strcpy(owner, LOCAL_ID);
  for (unsigned i = 0; i < c->member_count; ++i)
    if (strcmp(c->members[i].id, owner) < 0) strcpy(owner, c->members[i].id);
  if (!mqtt_format(safe, safe_size, "%s", root)) return false;
  for (char *p = safe; *p; ++p) if (*p == ':') *p = '_';
  if (account->mode) {
    const char *leaf = strrchr(root, '/');
    if (!mqtt_hex(safe, safe_size, leaf ? leaf + 1 : root)) return false;
    return mqtt_format(id, id_size, "v2_%s_%s_%s", account->key,
                       c->group[0] ? "group" : owner, safe);
  }
  return mqtt_format(id, id_size, "%s", root);
}
static const char *virtual_device_name(const feature_config_t *c,
                                       const a1_config_t *device) {
  return c->group[0] ? c->group : (device->name[0] ? device->name : "Phicomm A1");
}
bool feature_ha_device(char *id, size_t id_size, char *name, size_t name_size,
                       bool *grouped) {
  feature_config_t *c = malloc(sizeof(*c));
  if (!c) return false;
  feature_config_get(c);
  /* Called only by the MQTT owner, like discovery_virtual. */
  static mqtt_account_t account;
  static a1_config_t device;
  static char root[FEATURE_TOPIC_MAX], safe[FEATURE_TOPIC_MAX];
  char owner[13];
  account = mqtt_snapshot();
  a1_config_get(&device);
  root_for(c, 0, root);
  bool ok = virtual_identity(c, root, &account, owner, safe, sizeof(safe), id, id_size) &&
            mqtt_format(name, name_size, "%s", virtual_device_name(c, &device));
  *grouped = c->group[0] || c->member_count;
  free(c);
  return ok;
}
/* Capability persists through stale readings; only the configured peer owns its slot. */
static unsigned discovery_sensors(const feature_config_t *c) {
  unsigned mask=0;
  lock();
  for(unsigned i=0;i<c->peer_count && i<8;i++) {
    if(strcmp(c->peers[i].id,configuration.peers[i].id)) continue;
    if(samples[i].co2_supported) mask |= 1u<<i;
    if(samples[i].scd40_aux_supported) mask |= 1u<<(i+8);
  }
  unlock();
  return mask;
}
static void discovery_virtual(int (*publish)(const char *, const char *, bool),
                              const feature_config_t *c, const char *root,
                              bool remove) {
  const char *keys[] = {"fan",           "light",       "child_lock",
                        "led_always_on", "temperature", "humidity",
                        "pm25",          "formaldehyde", "co2", "scd40_temperature", "scd40_humidity"};
  const char *components[] = {"fan",    "light",  "switch", "switch",
                              "sensor", "sensor", "sensor", "sensor", "sensor", "sensor", "sensor"};
  const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮",
                         "温度", "湿度", "PM2.5", "甲醛", "二氧化碳", "SCD40 温度", "SCD40 湿度"};
  const char *icons[] = {"mdi:air-purifier", "mdi:led-strip-variant", "mdi:lock",
                         "mdi:led-on", NULL, NULL, NULL, "mdi:molecule", "mdi:molecule-co2", NULL, NULL};
  /* MQTT-owner-only scratch: publishing may dispatch callbacks on this stack. */
  static mqtt_account_t account;
  account = mqtt_snapshot();
  /* The MQTT owner publishes discovery; keep the device snapshot off its stack. */
  static a1_config_t device_config;
  a1_config_get(&device_config);
  const char *device_name = virtual_device_name(c, &device_config);
  char owner[13], device_id[192];
  static char safe[FEATURE_TOPIC_MAX];
  if (!virtual_identity(c, root, &account, owner, safe, sizeof(safe),
                        device_id, sizeof(device_id))) return;
  unsigned sensors=discovery_sensors(c);
  unsigned sets = c->peer_count > 1 ? c->peer_count + 1 : 1;
  for (unsigned set = 0; set < sets; set++)
    for (unsigned i = 0; i < 11; i++) {
      unsigned bits=i>=9 ? sensors>>8 : sensors & 255;
      bool supported=i<8 || (set ? (bits & (1u<<(set-1))) != 0 : bits != 0);
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
      if (remove || !supported) {
        if(remove || (published_sensors & (i>=9 ? 0xff00u : 0xffu))) publish(path, "", true);
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
      json_build_array_add(ids, json_object_new_string(device_id));
      json_build_object_add(device, "identifiers", ids);
      js(device, "name", device_name);
      js(device, "manufacturer", "Phicomm");
      js(device, "model", "A1 Air Purifier (EMW3080B)");
      json_build_object_add(o, "device", device);
      js(o, "unique_id", uid);
      json_build_object_add(o,"retain",json_object_new_boolean(false));
      js(o, "name", names[i]);
      if (icons[i]) js(o, "icon", icons[i]);
      static char topic[FEATURE_TOPIC_MAX];
      if (i < 4) {
        mqtt_format(topic, sizeof(topic), "%s/command", root);
        js(o, "command_topic", topic);
        mqtt_format(topic, sizeof(topic), "%s/%s/state", root, keys[i]);
        js(o, "state_topic", topic);
        if (i == 0) {
          mqtt_format(topic, sizeof(topic), "%s/command", root);
          js(o, "percentage_command_topic", topic);
          js(o,"preset_mode_command_topic",topic);
          js(o,"preset_mode_command_template","{\"action\":\"fan.mode.set\",\"params\":{\"mode\":{{ value | tojson }}}}");
          mqtt_format(topic,sizeof(topic),"%s/fan/mode/state",root);
          js(o,"preset_mode_state_topic",topic);
          json_object *modes=json_object_new_array();
          json_build_array_add(modes,json_object_new_string("auto"));
          json_build_array_add(modes,json_object_new_string("manual"));
          json_build_object_add(o,"preset_modes",modes);
          js(o,"command_template","{\"action\":\"fan.set\",\"params\":{\"on\":{{ 'true' if value == 'ON' else 'false' }}}}");
          js(o,"percentage_command_template","{\"action\":\"fan.set\",\"params\":{\"mode\":\"manual\",\"speed\":{{ value | int }}}}");
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
           (i == 4 || i == 9) ? "°C"
           : (i == 5 || i == 10) ? "%"
           : i == 6 ? "µg/m³"
           : i == 8 ? "ppm"
                    : "mg/m³");
        if (i != 7)
          js(o, "device_class", i==8 ? "carbon_dioxide" : i==9 ? "temperature" : i==10 ? "humidity" : keys[i]);
      }
      mqtt_leaf(&account,"state",topic,sizeof(topic));js(o,"json_attributes_topic",topic);
      js(o,"json_attributes_template","{{ {\"ip\": value_json.ip, \"firmware\": value_json.version} | tojson }}");
      if(i==2 || i==3)js(o,"entity_category","config");
      json_object *availability = json_object_new_array();
      if(i>=4) {
        json_object *node=json_object_new_object();
        js(node,"topic",sensor);
        mqtt_format(topic,sizeof(topic),"{{ 'online' if value_json.%s else 'offline' }}",i>=9?"scd40_aux_valid":i==8?"co2_valid":"valid");
        js(node,"value_template",topic);
        json_build_array_add(availability,node);
      }
      for (unsigned member = 0; i<4 && member <= c->member_count; member++) {
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
  unsigned sensor_mask=discovery_sensors(c);
  if ((!mqtt_online || mqtt_revision != c->revision || published_sensors != sensor_mask ||
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
      published_sensors=sensor_mask;
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
      direct_leaf(publish,prefix,"fan/mode/state",feature_fan_auto()?"auto":"manual",true);
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
        {
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
