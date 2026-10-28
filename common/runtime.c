#include "watchdog.h"
#include "mqtt/queue.h"
#include "mqtt/service.h"
#include "automation.h"
#include "json_owned.h"
#include "printbuf.h"
#include "phicomm_features.h"
#include "sensor_field.h"
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
#include "config_mp.inc"
#include "service_mp.h"
#include "engine.c"
static mico_mutex_t feature_lock;
static mico_mutex_t namespace_lock;
static mqtt_account_t feature_account;
static uint32_t mqtt_generation;
static int publish_error;
static mqtt_account_t mqtt_snapshot(void) {
  mqtt_account_t a;
  mico_rtos_lock_mutex(&namespace_lock); a = feature_account;
  mico_rtos_unlock_mutex(&namespace_lock); return a;
}
static feature_config_t configuration;
static feature_sample_t local_sample;
#ifdef FEATURE_DEVICE_A1
static feature_sample_t samples[FEATURE_MAX_PEERS];
#endif
static feature_pid_t pid;
static void reset_controller(bool preserve_gate);
static uint32_t boot_id, serial, announce_at;
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
static unsigned data_fields;
static unsigned available;
#ifdef FEATURE_DEVICE_A1
typedef struct {
  uint32_t request, sent, recover_at, auth_at, retired_boot[4];
  unsigned failures;
  bool lan, pending, authorized, auth_sent;
  char ip[16];
  uint8_t session_token[16],request_token[16];
  uint32_t last_seen;
} peer_runtime_t;
static peer_runtime_t peers[FEATURE_MAX_PEERS];
#else
#define SENSOR_SESSION_IDLE_MS (30u * 60u * 1000u)
typedef struct {
  char id[13], ip[16];
  uint8_t token[16];
  uint32_t boot, last_seen, last_read;
  bool valid, active;
} sensor_session_t;
static sensor_session_t sensor_sessions[FEATURE_MAX_PEERS];
#endif
static uint32_t member_seen[8];
static uint32_t mqtt_revision;
static bool mqtt_online;
#ifdef FEATURE_DEVICE_A1
static uint32_t mqtt_tick;
static unsigned mqtt_leaf_pending;
static uint8_t mqtt_telemetry_cursor;
/* Owner-only resumable publication positions. No bodies or JSON are retained. */
static unsigned mqtt_discovery_stage, mqtt_discovery_cursor, mqtt_domain_cursor;
static uint32_t mqtt_discovery_revision;
static bool mqtt_discovery_pending;
static bool was_coordinator;
static char published_root[FEATURE_TOPIC_MAX], published_ids[FEATURE_MAX_PEERS][13];
/* Only HA identity/cleanup fields survive a completed discovery pass. Never
 * retain credentials, automation or the 1408-byte authoritative config copy. */
typedef struct {
  char group[33];
  feature_member_t members[FEATURE_STORAGE_PEERS];
  char peer_ids[FEATURE_MAX_PEERS][13];
  uint8_t member_count,peer_count;
} discovery_layout_t;
static discovery_layout_t *published_layout;
static uint32_t mqtt_discovery_retry_at;
static bool mqtt_discovery_deferred,mqtt_capture_deferred;
static void discovery_layout_capture(discovery_layout_t *out,const feature_config_t *c) {
  memcpy(out->group,c->group,sizeof(out->group));
  memcpy(out->members,c->members,sizeof(out->members));
  out->member_count=c->member_count;out->peer_count=c->peer_count;
  for(unsigned i=0;i<c->peer_count;i++)memcpy(out->peer_ids[i],c->peers[i].id,13);
}
static void discovery_resource_wait(void) {
  mqtt_discovery_deferred=true;mqtt_discovery_retry_at=mico_rtos_get_time();
}
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
  if (!o) return false;
  char prefix[160];uint32_t generation;
  mico_rtos_lock_mutex(&namespace_lock);
  mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",feature_account.key);
  generation=mqtt_generation;
  mico_rtos_unlock_mutex(&namespace_lock);
  size_t prefix_length=strlen(prefix);
  if(strncmp(t,prefix,prefix_length) || strlen(t+prefix_length)>=128)return false;
  const char *wire=json_object_to_json_string(o);
  if(!wire)return false;
  return ph_mqtt_enqueue_send_generation(t,wire,strlen(wire),1,retain,generation)==0;
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
  return (unsigned)json_object_get_int64(json_object_object_get(o, k));
}
static const char *string(json_object *o, const char *k) {
  json_object *v = json_object_object_get(o, k);
  return v && json_object_is_type(v, json_type_string)
             ? json_object_get_string(v)
             : "";
}
static void number(json_object *o, const char *k, unsigned n) {
  /* MiCO JSON emits signed 32-bit counters; readers preserve their bits. */
  json_build_object_add(o, k, json_object_new_int((int32_t)n));
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
static void group_config_changed(const feature_config_t *,
                                 const feature_config_t *);
static bool install_config(feature_config_t *c) {
  if (c->peer_count > FEATURE_MAX_PEERS) return false;
#ifdef FEATURE_DEVICE_A1
  /* Legacy fields remain in the frozen Flash prefix, but sensor transport is LAN-only. */
  for (unsigned i=0;i<c->peer_count;i++) { c->peers[i].force_mqtt=0; c->peers[i].kind=FEATURE_M1; }
#endif
  if (!feature_config_save(c))
    return false;
  c->revision++;
#ifdef FEATURE_DEVICE_A1
  struct mapping {
    feature_sample_t samples[FEATURE_MAX_PEERS];
    peer_runtime_t peers[FEATURE_MAX_PEERS];
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
    memcpy(samples, next->samples, sizeof(samples));
    memcpy(peers, next->peers, sizeof(peers));
  } else {
    memset(samples, 0, sizeof(samples));
    memset(peers, 0, sizeof(peers));
  }
#else
  lock();
#endif
  group_config_changed(&configuration, c);
  bool preserve_gate = configuration.linkage.enabled && c->linkage.enabled &&
                       configuration.linkage.hysteresis_enabled && c->linkage.hysteresis_enabled;
  configuration = *c;
  automatic_immediate = true;
  automatic_valid = false;
  reset_controller(preserve_gate);
  unlock();
#ifdef FEATURE_DEVICE_A1
  free(next);
#endif
  service_cache_changed(SERVICE_CACHE_ALL);
  return true;
}
static void sensor_decode(json_object *o, feature_sample_t *s) {
  const char *names[]={"temperature","humidity","pm25","formaldehyde","co2"};
  const char *units[]={"°C","%","µg/m³","mg/m³","ppm"};
  json_object *values=json_object_object_get(o,"sensors");
  memset(s,0,sizeof(*s));s->partial=true;s->received_ms=s->co2_received_ms=now_ms();
  for(unsigned f=0;f<5;f++) {
    json_object *field=json_object_object_get(values,names[f]);
    /* UDP v2 permits its historical body; numeric MQTT schema2 requires
     * an explicit descriptor. Unknown future types never become numbers. */
    if(values && integer(o,"schema")==2){
      json_object *name=json_object_object_get(field,"name"),*unit=json_object_object_get(field,"unit");
      if(strcmp(string(field,"type"),"number") || json_object_get_string_len(json_object_object_get(field,"type"))!=6 ||
         !sensor_field_text(string(field,"name"),name?json_object_get_string_len(name):0,1,SENSOR_FIELD_NAME_MAX) ||
         !sensor_field_text(string(field,"unit"),unit?json_object_get_string_len(unit):0,0,SENSOR_FIELD_UNIT_MAX))continue;
    }
    json_object *v=json_object_object_get(values?field:o,names[f]);
    if(values)v=json_object_object_get(field,"value");
    json_object *age=json_object_object_get(values?field:o,values?"age_ms":f==4?"co2_age_ms":"age_ms");
    bool enabled=values?integer(field,"valid"):integer(o,f==4?"co2_valid":"valid");
    if(f==4)s->co2_supported=values?field!=NULL:integer(o,"co2_supported");
    if(!enabled || (values&&(!json_object_is_type(json_object_object_get(field,"valid"),json_type_boolean) ||
       strcmp(string(field,"unit"),units[f]) || json_object_get_string_len(json_object_object_get(field,"unit"))!=(int)strlen(units[f]))) || !age || !json_object_is_type(age,json_type_int) ||
       json_object_get_int64(age)<0 || json_object_get_int64(age)>=(f==4?20000u:FEATURE_STALE_MS) ||
       (!json_object_is_type(v,json_type_int)&&!json_object_is_type(v,json_type_double)))continue;
    float value=json_object_get_double(v);
    if(!isfinite(value)||(f==0&&(value < -40 || value>125))||(f==1&&(value<0||value>100))||(f>=2&&value<0)||(f==4&&(value==0||value>65535)))continue;
    if(f==4){s->co2=value;s->co2_valid=true;s->co2_age_ms=integer(values?field:o,values?"age_ms":"co2_age_ms");}
    else {s->fields|=1u<<f;s->value[f]=value;s->field_age_ms[f]=json_object_get_int(age);if((unsigned)json_object_get_int(age)>s->age_ms)s->age_ms=json_object_get_int(age);}
  }
  s->valid=s->fields!=0;s->boot=integer(o,"boot");s->sequence=integer(o,"sequence");
}
static void co2_body(json_object *o, const feature_sample_t *s, uint32_t now) {
  json_build_object_add(o,"co2_supported",json_object_new_boolean(s->co2_supported));
  json_build_object_add(o,"co2_valid",json_object_new_boolean(feature_co2_fresh(s,now)));
  number(o,"co2_age_ms",s->co2_age_ms+(uint32_t)(now-s->co2_received_ms));
  if(feature_co2_fresh(s,now)) json_build_object_add(o,"co2",json_object_new_double(s->co2));
  else json_object_object_add(o,"co2",NULL);
}
static json_object *sample_body(const feature_sample_t *s, uint32_t now) {
  json_object *o = json_object_new_object();
  if (!o)
    return NULL;
  co2_body(o,s,now);
#ifdef FEATURE_DEVICE_A1
  number(o, "sequence", s->sequence);
  number(o, "boot", s->boot);
#else
  /* Typed MQTT measurements use positive uint32 counters. Keep the UDP
   * control envelope's historical bit-preserving representation separate. */
  json_build_object_add(o,"sequence",json_object_new_int64(s->sequence));
  json_build_object_add(o,"boot",json_object_new_int64(s->boot));
#endif
  number(o, "age_ms", s->age_ms + (uint32_t)(now - s->received_ms));
  json_build_object_add(o, "valid",
                         json_object_new_boolean(feature_sample_fresh(s, now)));
  const char *keys[] = {"temperature", "humidity", "pm25", "formaldehyde"};
  for (unsigned i = 0; i < 4; i++)
    json_object_object_add(o, keys[i], (feature_sample_fresh_fields(s,now)&(1u<<i))?json_object_new_double(s->value[i]):NULL);
  #ifndef FEATURE_DEVICE_A1
  js(o,"model","m1");
  json_object *values=json_object_new_object();
  const char *units[]={"°C","%","µg/m³","mg/m³","ppm"};
  for(unsigned i=0;i<5;i++) {
    if(i==4&&!s->co2_supported)continue;
    bool fresh=i==4?feature_co2_fresh(s,now):(feature_sample_fresh_fields(s,now)&(1u<<i))!=0;
    json_object *field=json_object_new_object();
    js(field,"type","number");js(field,"name",i==4?"co2":keys[i]);
    js(field,"unit",units[i]);json_build_object_add(field,"valid",json_object_new_boolean(fresh));
    number(field,"age_ms",i==4?s->co2_age_ms+(uint32_t)(now-s->co2_received_ms):s->age_ms+(uint32_t)(now-s->received_ms));
    json_object_object_add(field,"value",fresh?json_object_new_double(i==4?s->co2:s->value[i]):NULL);
    json_build_object_add(values,i==4?"co2":keys[i],field);
  }
  json_build_object_add(o,"sensors",values);
  #endif
  return o;
}
#include "lan_client_adapter.inc"
#include "lan_sensor.inc"
#include "feature_mp.h"
#include "group_mp.inc"
#ifndef FEATURE_DEVICE_A1
/* Only the MQTT command adapter invokes this grant; no UDP/HTTP grant path. */
bool feature_sensor_session_mp(const ph_value *o,const char **code) {
  char id[13];uint8_t token[16];const void *raw;size_t bytes;uint32_t version,boot;ph_value v;
  *code="invalid_params";
  if(!o||o->kind!=PHV_MAP||!phv_keys(o,"|v||a1||boot||token|")||!lan_u32(o,"v",&version)||version!=3||!lan_u32(o,"boot",&boot)||!boot||!lan_id(o,"a1",id)||!strcmp(id,LOCAL_ID)||!phv_get(o,"token",&v)||!phv_bytes(&v,&raw,&bytes)||bytes!=16)return false;
  unsigned bits=0;for(unsigned k=0;k<16;k++)bits|=((const uint8_t*)raw)[k];if(!bits)return false;memcpy(token,raw,16);
  uint32_t now=now_ms();unsigned slot=FEATURE_MAX_PEERS;
  lock();
  for(unsigned i=0;i<FEATURE_MAX_PEERS;i++){
    sensor_session_t *p=&sensor_sessions[i];
    if(p->valid&&(uint32_t)(now-p->last_seen)>=SENSOR_SESSION_IDLE_MS)p->valid=false;
    if(p->valid&&!strcmp(p->id,id)){slot=i;break;}
    if(!p->valid&&slot==FEATURE_MAX_PEERS)slot=i;
  }
  if(slot==FEATURE_MAX_PEERS){unlock();*code="busy";return false;}
  sensor_session_t *p=&sensor_sessions[slot];bool same=p->valid&&p->boot==boot&&!memcmp(p->token,token,16);
  if(!same)memset(p,0,sizeof(*p));strcpy(p->id,id);memcpy(p->token,token,16);p->boot=boot;p->last_seen=now;p->valid=true;
  unlock();*code="accepted";return true;
}
#endif
bool feature_bound(void) {
  lock(); bool b=false;
#ifdef FEATURE_DEVICE_A1
  b=configuration.group[0] || configuration.peer_count;
#else
  for(unsigned i=0;i<FEATURE_MAX_PEERS;i++)b|=sensor_sessions[i].valid && (uint32_t)(now_ms()-sensor_sessions[i].last_seen)<SENSOR_SESSION_IDLE_MS;
#endif
  unlock();return b;
}
#ifdef FEATURE_DEVICE_A1
static bool data_bound(void) { lock();bool b=configuration.peer_count!=0;unlock();return b; }
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
  service_cache_changed(SERVICE_CACHE_STATUS);
}
#ifdef FEATURE_DEVICE_A1
void feature_values_snapshot(feature_values_t *out) {
  lock();
  memcpy(out->value,aggregate,sizeof(out->value));
  out->co2=aggregate_co2;
  out->sample_count=available;out->co2_sample_count=available_co2;
  out->configured_count=configuration.peer_count;
  out->statistic=configuration.linkage.statistic;
  out->valid=data_valid;
  unlock();
}
#endif
void feature_clock_valid(void) { clock_ok = true; service_cache_changed(SERVICE_CACHE_STATUS); }
#include "feature_mp.inc"
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
  bool changed = effective_speed != *speed || effective_on != *on;
  effective_speed = *speed;
  automatic_on = *on;
  effective_on = *on;
  unlock();
  if(changed)service_cache_changed(SERVICE_CACHE_STATUS);
}
void feature_fan_snapshot(bool *on,unsigned *speed,bool *automatic,unsigned *limit)
{
    lock();
    /* Observe applied controller state; a read must never advance PID. */
    *on=effective_on; *speed=effective_speed;
    *automatic=configuration.linkage.enabled;
    *limit=automatic_night>=0 && (unsigned)automatic_night<configuration.sleep_count
        ? configuration.sleeps[automatic_night].max_speed : configuration.linkage.max_speed;
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
int feature_fan_command_mp(const ph_value *o) {
 unsigned selection=0;ph_value mode,speed;int64_t n=-1;
 if(phv_get(o,"mode",&mode)){
  if(phv_equal_text(&mode,"auto"))selection=2;
  else if(phv_equal_text(&mode,"manual"))selection=1;else return 0;
 }
 if(phv_get(o,"speed",&speed)&&(!phv_int(&speed,&n)||n<0||n>100))return 0;
 return feature_fan_apply(selection,(int)n);
}
int feature_fan_apply(unsigned mode, int speed) {
  if(mode>3 || speed < -1 || speed>100)return 0;
  /* Power-only/manual-speed actions need no feature snapshot or allocation. */
  lock();bool current=configuration.linkage.enabled;unlock();
  if((!mode || mode==(current?2u:1u)) && (!current || speed<=0))return current?2:1;
  feature_config_t *c = malloc(sizeof(*c)); if (!c) return 0;
  feature_config_get(c);
  bool automatic = c->linkage.enabled;
  if(mode)automatic=mode==3?!automatic:mode==2;
  bool changed = automatic != (bool)c->linkage.enabled;
  c->linkage.enabled = automatic;
  if (automatic && speed>0) {
    unsigned maximum=speed;
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
bool feature_group_active(void) {
  lock();bool active=configuration.group[0] && configuration.member_count;unlock();return active;
}
int feature_light_select_mp(unsigned effect,ph_writer *response) {
 if(effect>EFFECT_COLOR_BREATHE&&effect!=255)return 400;
 feature_config_t *c=malloc(sizeof(*c));if(!c)return 500;
 feature_config_get(c);c->light.effect=effect==255?(c->light.effect+1)%(EFFECT_COLOR_BREATHE+1):effect;
 unsigned selected=c->light.effect;bool ok=install_config(c);unsigned revision=c->revision;free(c);
 if(ok&&feature_group_active()){
  unsigned char bytes[32];ph_buf buffer;ph_writer w;
  if(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes))){
   phw_begin(&w,NULL,false);phw_uint(&w,"effect",selected);phw_end(&w);size_t n=phw_finish_fixed(&w);ph_value v;
   if(n&&phv_parse(PH_WIRE_MSGPACK,bytes,n,&v))group_broadcast_config_mp("light",&v);
  }
 }
 int status=ok?200:409;if(response)feature_result_mp(response,status,revision,NULL);return status;
}
unsigned feature_light_cap(unsigned brightness) {
  lock();
  if (sleep_state.light_sleep && brightness > sleep_state.brightness)
    brightness = sleep_state.brightness;
  unlock();
  return brightness;
}
#ifdef FEATURE_DEVICE_A1
unsigned feature_filter_weight(void) {
  lock();
  float ratio = 0;
  if ((data_fields & 4) && configuration.light.quality_pm > 0)
    ratio = fmaxf(ratio, aggregate[2] / configuration.light.quality_pm);
  if ((data_fields & 8) && configuration.light.quality_hcho > 0)
    ratio = fmaxf(ratio, aggregate[3] / configuration.light.quality_hcho);
  unlock();
  return ratio <= 1 ? 15 : ratio <= 2 ? 75 : 150;
}
#endif
void feature_light_frame(uint32_t out[32], unsigned brightness, bool enabled,
                         uint32_t color) {
  lock();
  feature_light_t l = configuration.light;
  float values[4];
  memcpy(values, aggregate, sizeof(values));
  bool valid[4] = {data_fields&1,data_fields&2,data_fields&4,data_fields&8};
  bool powered = effective_on;
  unsigned speed = powered ? effective_speed : 0;
  feature_sleep_state_t sleep = sleep_state;
  unlock();
  unsigned original_brightness = brightness;
  if (sleep.light_sleep && brightness > sleep.brightness)
    brightness = sleep.brightness;
  if (!powered || !enabled || !brightness) {
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
    double animation_ms = (double)now_ms() * feature_light_speed(&l) / 5;
    if (l.effect == EFFECT_BREATHE || l.effect == EFFECT_COLOR_BREATHE)
      b = brightness * (1 + sin(animation_ms / 2000.0)) / 2;
    for (unsigned i = 0; i < 32; i++) {
      unsigned r = (color >> 8) & 255, g = (color >> 16) & 255,
               bl = color & 255;
      if (l.effect == EFFECT_RAINBOW || l.effect == EFFECT_COLOR_BREATHE) {
        unsigned offset = l.effect == EFFECT_RAINBOW ? i * 768 / 32 : 0;
        unsigned p = (offset + (uint32_t)(animation_ms / 30)) % 768;
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
unsigned feature_mqtt_filters(char filters[][FEATURE_TOPIC_MAX], unsigned capacity) {
  mqtt_account_t a=mqtt_snapshot();unsigned n=0;
  feature_config_t *c=malloc(sizeof(*c));if(!c)return FEATURE_MQTT_FILTER_ERROR;feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
  unsigned required=2u+(c->peer_count || c->group[0] ? 1u : 0u)+(c->peer_count?1u:0u);
#else
  unsigned required=1;
#endif
  if(capacity<required){free(c);return FEATURE_MQTT_FILTER_ERROR;}
#ifdef FEATURE_DEVICE_A1
  mqtt_link_topic(&a,"a1",LOCAL_ID,"+","request",filters[n++],FEATURE_TOPIC_MAX);
  mqtt_aux(&a,"address","+",filters[n++],FEATURE_TOPIC_MAX);
  if(c->peer_count || c->group[0]) {char root[FEATURE_TOPIC_MAX];root_for(c,0,root);mqtt_format(filters[n++],FEATURE_TOPIC_MAX,"%s/#",root);}
  if(c->peer_count)mqtt_aux(&a,"sensor","+/state",filters[n++],FEATURE_TOPIC_MAX);
#else
  char leaf[32];snprintf(leaf,sizeof(leaf),"%s/command",LOCAL_ID);
  mqtt_aux(&a,"sensor",leaf,filters[n++],FEATURE_TOPIC_MAX);
#endif
  free(c);return n;
}
/* Reconnect only when the subscription set changes. Bound temporary storage
 * to the product's real maximum, outside the nested MQTT callback stack. */
bool feature_mqtt_filters_current(char filters[][FEATURE_TOPIC_MAX], unsigned count) {
#ifdef FEATURE_DEVICE_A1
  const unsigned capacity=FEATURE_MQTT_A1_FILTER_MAX;
#else
  const unsigned capacity=FEATURE_MQTT_M1_FILTER_MAX;
#endif
  if(count>capacity)return false;
  char (*next)[FEATURE_TOPIC_MAX] = calloc(capacity, FEATURE_TOPIC_MAX);
  if (!next) return false;
  unsigned n = feature_mqtt_filters(next,capacity);
  bool same = n == count;
  for (unsigned i = 0; same && i < n; ++i)
    same = !strcmp(filters[i], next[i]);
  free(next);
  return same;
}
static bool virtual_input(const char *t, const void *data, size_t n,
                          bool retained);
#ifndef FEATURE_DEVICE_A1
static __attribute__((noinline)) bool display_availability_input(const mqtt_account_t *account,
        const char *topic,const void *data,size_t length,bool retained)
{
  char prefix[MQTT_KEY_MAX+12];
  if(!mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",account->key))return false;
  size_t n=strlen(prefix);
  if(strncmp(topic,prefix,n))return false;
  const char *model=topic+n,*slash=strchr(model,'/');
  if(!slash || slash==model || slash-model>32 || strlen(slash+1)!=25 || strcmp(slash+13,"/availability"))return false;
  for(const char *p=model;p<slash;p++)if((unsigned char)*p<=0x20 || *p==0x7f || *p=='+' || *p=='#')return false;
  char id[13];
  memcpy(id,slash+1,12);id[12]=0;
  for(unsigned i=0;i<12;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
  /* An online announcement cannot renew a measurement lease. Retained
   * announcements also cannot restore cache state after reconnect. */
  if(!retained && length==7 && !memcmp(data,"offline",7))m1_display_source_offline(NULL,id);
  return true;
}
#endif
bool feature_mqtt_input(const char *t, const void *data, size_t n,
                        bool retained) {
  mqtt_account_t account = mqtt_snapshot();
#ifndef FEATURE_DEVICE_A1
  if(display_availability_input(&account,t,data,n,retained))return true;
  char grant_topic[FEATURE_TOPIC_MAX],leaf[32];snprintf(leaf,sizeof(leaf),"%s/command",LOCAL_ID);
  mqtt_aux(&account,"sensor",leaf,grant_topic,sizeof(grant_topic));
  if(!strcmp(t,grant_topic)) {
    if(!retained){ph_value root,action,params;if(phv_parse(PH_WIRE_MSGPACK,data,n,&root)&&phv_get(&root,"action",&action)&&phv_equal_text(&action,"sensor.session.set")&&phv_get(&root,"params",&params)){const char *code;feature_sensor_session_mp(&params,&code);}}return true;
  }
#endif
  char sensor_prefix[FEATURE_TOPIC_MAX];mqtt_aux(&account,"sensor","",sensor_prefix,sizeof(sensor_prefix));
  size_t prefix_len=strlen(sensor_prefix);
  if(!strncmp(t,sensor_prefix,prefix_len)){
    // Account-scoped read-only measurements. Model is deliberately absent.
    // Retained snapshots cannot prove freshness after reconnection.
    const char *suffix=t+prefix_len;char mac[13];
    if(retained || strlen(suffix)!=18 || strcmp(suffix+12,"/state"))return true;
    memcpy(mac,suffix,12);mac[12]=0;char normalized[13];if(!feature_mac(mac,normalized))return true;
    json_object *o=parse(data,n);if(!o)return true;
    json_object *schema=json_object_object_get(o,"schema"),*id=json_object_object_get(o,"device_id"),*boot=json_object_object_get(o,"boot"),*seq=json_object_object_get(o,"sequence");
    if(!schema || !json_object_is_type(schema,json_type_int)||json_object_get_int64(schema)!=2 ||
       !id || !json_object_is_type(id,json_type_string)||json_object_get_string_len(id)!=12||strcmp(string(o,"device_id"),normalized) ||
       !boot || !json_object_is_type(boot,json_type_int)||json_object_get_int64(boot)<1||json_object_get_int64(boot)>UINT32_MAX ||
       !seq || !json_object_is_type(seq,json_type_int)||json_object_get_int64(seq)<1||json_object_get_int64(seq)>UINT32_MAX){json_object_put(o);return true;}
#ifndef FEATURE_DEVICE_A1
    /* Numeric bindings are independent of the legacy linkage decoder's
     * known sensor subset. A new field/model needs no firmware adaptation. */
    m1_display_source_sample(normalized,o);
#endif
    feature_sample_t sample;sensor_decode(o,&sample);
    if(!sample.boot||!sample.sequence){json_object_put(o);return true;}
#ifdef FEATURE_DEVICE_A1
    lock();for(unsigned i=0;i<configuration.peer_count;i++)if(!strcmp(configuration.peers[i].id,normalized)){
      if(peers[i].lan && (uint32_t)(now_ms()-peers[i].last_seen)<FEATURE_STALE_MS)break;
      bool retired=false;for(unsigned k=0;k<4;k++)retired|=peers[i].retired_boot[k]==sample.boot;
      if(retired || (samples[i].partial && sample.boot==samples[i].boot && (int32_t)(sample.sequence-samples[i].sequence)<=0))break;
      if(samples[i].partial && samples[i].boot!=sample.boot){memmove(peers[i].retired_boot+1,peers[i].retired_boot,3*sizeof(uint32_t));peers[i].retired_boot[0]=samples[i].boot;}
      if(!peers[i].lan || (uint32_t)(now_ms()-peers[i].last_seen)>=FEATURE_STALE_MS)samples[i]=sample;
      break;
    }unlock();
#else
    m1_co2_source_sample(normalized,&sample);
#endif
    json_object_put(o);return true;
  }
  char address_prefix[192];
  mqtt_aux(&account,"address","",address_prefix,sizeof(address_prefix));
  size_t address_length = strlen(address_prefix);
  if (!strncmp(t,address_prefix,address_length)) {
#ifndef FEATURE_DEVICE_A1
    return false;
#else
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
#endif
  }
  char link_prefix[FEATURE_TOPIC_MAX];mqtt_leaf(&account,"link/",link_prefix,sizeof(link_prefix));
  if(!strncmp(t,link_prefix,strlen(link_prefix))) {
    const char *peer=t+strlen(link_prefix);char source[13];
    bool valid=strlen(peer)>=13;memcpy(source,peer,valid?12:0);source[12]=0;
    valid=valid && peer[12]=='/' && (!strcmp(peer+13,"request") || !strcmp(peer+13,"response"));
    ph_value root;char actual[13];
    if(valid&&phv_parse(PH_WIRE_MSGPACK,data,n,&root)&&group_text_source(&root,actual)&&!strcmp(actual,source))group_input_mp(&root,false,retained,NULL);
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
#ifndef FEATURE_DEVICE_A1
  m1_co2_source_reset();
  m1_display_source_mqtt_reset();
#endif
  mico_rtos_lock_mutex(&namespace_lock);
  bool changed=feature_account.mode!=account->mode || strcmp(feature_account.key,account->key) ||
      strcmp(feature_account.model,account->model) || strcmp(feature_account.mac,account->mac);
  feature_account = *account;
  mqtt_generation=ph_mqtt_send_generation();
  mico_rtos_unlock_mutex(&namespace_lock);
  lock();
  mqtt_online=false;
#ifdef FEATURE_DEVICE_A1
  for (unsigned i=0; i<8; ++i) {
    ph_buf_free(group_members[i].pending);
    group_members[i].pending=NULL;
  }
#endif
#ifdef FEATURE_DEVICE_A1
  /* A grant queued before a filter reconnect may have been discarded with
   * its owner generation. Retry unconfirmed sessions; healthy UDP stays quiet. */
  if(!changed)for(unsigned i=0;i<FEATURE_MAX_PEERS;i++)if(!peers[i].authorized){peers[i].auth_sent=false;peers[i].auth_at=0;}
  mqtt_tick=0;mqtt_leaf_pending=0;mqtt_telemetry_cursor=0;
  mqtt_discovery_pending=false;mqtt_discovery_deferred=false;mqtt_capture_deferred=false;
  if(changed) {
    memset(peers,0,sizeof(peers));memset(samples,0,sizeof(samples));data_valid=false;data_fields=0;
    published_root[0]=0; published_count=0;
    free(published_layout); published_layout=NULL;
  }
#else
  if(changed)memset(sensor_sessions,0,sizeof(sensor_sessions));
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
  return publish_error;
}
bool feature_mqtt_initial_ready(void) {
#ifdef FEATURE_DEVICE_A1
  return mqtt_online && !mqtt_discovery_pending;
#else
  return mqtt_online;
#endif
}
#ifdef FEATURE_DEVICE_A1
/* One initial grant; retries are driven by failed UDP reads and capped at once/minute. */
static bool sensor_grant(const feature_config_t *c,unsigned i,uint32_t now) {
  uint8_t token[16];bool empty=true;
  lock();for(unsigned k=0;k<16;k++)empty&=peers[i].session_token[k]==0;unlock();
  if(empty) {
    if(!new_token(token))return false;
    lock();if(!peer_current(c,i)){unlock();return false;}memcpy(peers[i].session_token,token,16);unlock();
  }
  char storage[256];ph_buf buffer;ph_writer writer;
  if(!phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage)))return false;
  lock();memcpy(token,peers[i].session_token,16);unlock();
  phw_begin(&writer,NULL,false);phw_text(&writer,"action","sensor.session.set");phw_begin(&writer,"params",false);
  phw_uint(&writer,"v",3);phw_text(&writer,"a1",LOCAL_ID);phw_u32(&writer,"boot",boot_id);phw_bytes_value(&writer,"token",token,16);phw_end(&writer);phw_end(&writer);
  size_t length=phw_finish_fixed(&writer);if(!length)return false;
  char topic[FEATURE_TOPIC_MAX];mqtt_account_t account=mqtt_snapshot();char leaf[32];snprintf(leaf,sizeof(leaf),"%s/command",c->peers[i].id);
  bool sent=false;
  if(mqtt_aux(&account,"sensor",leaf,topic,sizeof(topic))){uint32_t generation;mico_rtos_lock_mutex(&namespace_lock);generation=mqtt_generation;mico_rtos_unlock_mutex(&namespace_lock);sent=ph_mqtt_enqueue_send_generation(topic,storage,length,1,false,generation)==0;}
  lock();if(peer_current(c,i)) { peers[i].auth_at=now;peers[i].auth_sent=sent; }unlock();
  return sent;
}
static void sensor_poll(const feature_config_t *c,unsigned i,uint32_t now,bool mqtt_connected) {
  char ip[16];bool grant,due,timed_out=false;unsigned failures;
  lock();
  if(!peer_current(c,i)){unlock();return;}
  if(peers[i].pending && (uint32_t)(now-peers[i].sent)>=c->peers[i].timeout_ms) {
    peers[i].pending=false;peers[i].lan=false;timed_out=true;
    if(peers[i].failures<6)peers[i].failures++;
    peers[i].recover_at=now+(peers[i].failures==1?1000u:peers[i].failures==2?5000u:peers[i].failures==3?15000u:60000u);
  }
  failures=peers[i].failures;
  grant=mqtt_connected && ((!peers[i].auth_sent && (!peers[i].auth_at || (uint32_t)(now-peers[i].auth_at)>=5000u)) || (failures && (uint32_t)(now-peers[i].auth_at)>=60000u));
  due=!peers[i].pending && (peers[i].lan ? (uint32_t)(now-peers[i].sent)>=5000u :
      (!peers[i].sent || ((int32_t)(now-peers[i].recover_at)>=0 && (c->peers[i].recovery || failures==0))));
  snprintf(ip,sizeof(ip),"%s",c->peers[i].ip[0]?c->peers[i].ip:peers[i].ip);
  unlock();
  if(timed_out&&failures>=2)lan_client_invalidate(c->peers[i].id);
  uint8_t local_token[16];bool local_ready=lan_client_token(c->peers[i].id,ip,2u,local_token);
  if(!local_ready&&grant)sensor_grant(c,i,now);
  lock();bool ready=local_ready||peers[i].auth_sent;unlock();if(!due || !ready)return;
  /* An explicit address wins. A learned address is retried before broadcast discovery. */
  if(!ip[0] || (!c->peers[i].ip[0] && failures>=2))strcpy(ip,"255.255.255.255");
  unsigned request=next_request();lan_packet packet={0};packet.op=LAN_READ;strcpy(packet.src,LOCAL_ID);strcpy(packet.dst,c->peers[i].id);packet.boot=boot_id;packet.id=request;lock();memcpy(packet.token,local_ready?local_token:peers[i].session_token,16);memcpy(peers[i].request_token,packet.token,16);unlock();
  /* Publish pending before sendto so an immediate reply cannot race its request slot. */
  lock();if(!peer_current(c,i)){unlock();return;}
  peers[i].request=request;peers[i].sent=now;peers[i].pending=true;unlock();
  bool sent=lan_send(ip,&packet);
  if(!sent) { lock();if(peer_current(c,i))peers[i].sent=now-c->peers[i].timeout_ms;unlock(); }
}
#endif
#ifdef PHICOMM_DEVICE_OTA
static bool worker_running, worker_stop, worker_exiting, worker_ready;
bool feature_running(void){lock();bool yes=worker_running;unlock();return yes;}
static bool feature_worker_should_stop(void){
  lock();bool stop=worker_stop;
  if(stop){worker_exiting=true;watchdog_binding_enable(false);}
  unlock();return stop;
}
#endif
static void feature_worker(mico_thread_arg_t arg) {
  UNUSED_PARAMETER(arg);
#ifdef FEATURE_DEVICE_A1
  /* Restart after OTA must never replay a press from the suspended worker. */
  a1_input_clear();
#endif
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
#ifdef FEATURE_DEVICE_A1
    int broadcast=1;setsockopt(udp,SOL_SOCKET,SO_BROADCAST,&broadcast,sizeof(broadcast));
#endif
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
    if(feature_worker_should_stop())break;
#endif
#ifdef FEATURE_DEVICE_A1
    a1_input_poll();
#endif
    uint32_t now = now_ms();
    feature_config_get(c);
#ifdef FEATURE_DEVICE_A1
    struct { bool mqtt_connected; char ip[16]; } state;
    mico_rtos_lock_mutex(&a1_lock);
    state.mqtt_connected=a1_state.mqtt_connected;memcpy(state.ip,a1_state.ip,sizeof(state.ip));
    mico_rtos_unlock_mutex(&a1_lock);

#else
    struct {
      m1_sample_t sample;
      struct { bool ready, received; uint16_t co2_ppm; uint32_t updated_ms, samples; } scd40;
    } state;
    mico_rtos_lock_mutex(&m1_lock);
    state.sample=m1_state.sample;
    state.scd40.ready=m1_state.scd40.ready;state.scd40.received=m1_state.scd40.received;
    state.scd40.co2_ppm=m1_state.scd40.co2_ppm;state.scd40.updated_ms=m1_state.scd40.updated_ms;
    state.scd40.samples=m1_state.scd40.samples;
    mico_rtos_unlock_mutex(&m1_lock);
    feature_sample_t local;
    memset(&local, 0, sizeof(local));
    local.value[0] = state.sample.temperature_deci_c / 10.0f;
    local.value[1] = state.sample.humidity_deci_pct / 10.0f;
    local.value[2] = state.sample.pm25_ug_m3;
    local.value[3] = state.sample.hcho_ug_m3 / 1000.0f;
    local.co2_supported=state.scd40.ready || state.scd40.received;
    local.co2_valid=state.scd40.ready && state.scd40.received;
    local.co2=state.scd40.co2_ppm; local.co2_received_ms=state.scd40.updated_ms;
    static uint32_t wire_sequence, uart_sequence, co2_sequence;
    static bool co2_ready;
    if(!wire_sequence || uart_sequence!=state.sample.sequence || co2_sequence!=state.scd40.samples || co2_ready!=state.scd40.ready) {
      if(!++wire_sequence)++wire_sequence;
      uart_sequence=state.sample.sequence; co2_sequence=state.scd40.samples; co2_ready=state.scd40.ready;
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
    group_poll(c,now);
    for(unsigned i=0;i<c->peer_count;i++)sensor_poll(c,i,now,state.mqtt_connected);
    if ((uint32_t)(now - tick_at) >= 5000) {
      tick_at = now;
      feature_sample_t current[FEATURE_MAX_PEERS];
      lock();
      memcpy(current, samples, sizeof(current));
      unlock();
      float v[4] = {0}, co2=0;
      unsigned nc=feature_co2_aggregate(current,c->peer_count,now,c->linkage.statistic,&co2);
      unsigned n = feature_aggregate(current, c->peer_count, now,
                                     c->linkage.statistic, v, NULL);
      lock();
      available = n;
      available_co2=nc; aggregate_co2=co2;
      data_fields=0;for(unsigned f=0;f<4;f++)if(n && isfinite(v[f]))data_fields|=1u<<f;
      unsigned required=((c->linkage.metrics&1)?4u:0u)|((c->linkage.metrics&2)?8u:0u);
      data_valid = (data_fields&required)==required && (!(c->linkage.metrics & 4) || nc > 0);
      memcpy(aggregate,v,sizeof(v));
      if (!data_valid) { reset_controller(configuration.linkage.hysteresis_enabled); pid.condition=0; }
      automatic_valid = false;
      unlock();
      service_cache_changed(SERVICE_CACHE_STATUS);
    }
#endif
    lan_client_poll();
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
          char ip[16];mqtt_format(ip,sizeof(ip),"%s",inet_ntoa(from.sin_addr));
          ph_value request;
          if(phv_parse(PH_WIRE_MSGPACK,wire,(size_t)n,&request)){
            /* Pair operations borrow the datagram only during this call. */
            ph_value operation;bool paired=lan_client_response(&request,ip);
            bool auth_op=phv_get(&request,"op",&operation)&&(phv_equal_text(&operation,"auth_pair")||phv_equal_text(&operation,"auth_resume")||phv_equal_text(&operation,"auth_unpair"));
            char *response=auth_op&&!paired?malloc(FEATURE_WIRE_MAX):NULL;
            if(response){ph_buf buf;ph_writer writer;
              if(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buf,response,FEATURE_WIRE_MAX)&&feature_lan_pair(&request,&writer,ip)){
                size_t bytes=phw_finish_fixed(&writer);
                if(bytes)sendto(udp,response,bytes,0,(struct sockaddr*)&from,length);
                paired=true;
              }free(response);
            }
            if(!paired&&!auth_op&&!group_input_mp(&request,true,false,ip)){lan_packet packet;if(lan_decode_value(&request,&packet))lan_sensor_input(&packet,ip);}
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

#ifdef FEATURE_DEVICE_A1
  for(unsigned i=0;i<8;i++){
    ph_buf_free(group_members[i].pending);group_members[i].pending=NULL;
  }
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
/* One attempt only: callers may retry after cleanup/OOM without holding an
 * HTTP or MQTT owner in a wait loop. worker_running reserves thread creation. */
OSStatus feature_resume_step(void){
  lock();
  if(worker_exiting){unlock();return kInProgressErr;}
  if(worker_running){bool ready=worker_ready;worker_stop=false;unlock();return ready?kNoErr:kInProgressErr;}
  worker_stop=worker_ready=false;worker_running=true;unlock();watchdog_binding_enable(true);
  OSStatus e=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"binding",feature_worker,FEATURE_WORKER_STACK_SIZE,0);
  if(e!=kNoErr){watchdog_binding_enable(false);lock();worker_running=false;unlock();return e;}
  lock();bool ready=worker_ready,running=worker_running;unlock();
  return ready?kNoErr:running?kInProgressErr:kNoMemoryErr;
}
OSStatus feature_resume(void){
  /* Boot may wait for readiness, but never indefinitely for acknowledged exit. */
  uint32_t start=now_ms();
  for(;;){
    OSStatus e=feature_resume_step();if(e!=kInProgressErr)return e;
    if((uint32_t)(now_ms()-start)>=5000)return kNoMemoryErr;
    mico_thread_msleep(20);
  }
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
  e = mico_rtos_init_mutex(&lan_client_lock);
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
#ifndef FEATURE_DEVICE_A1
  if(!boot_id)boot_id=1;
#endif
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
static int direct_leaf(int (*p)(const char *, const char *, bool),
                        const char *root, const char *leaf, const char *body,
                        bool retain) {
  if (!body) return -1;
  char topic[FEATURE_TOPIC_MAX];
  mqtt_format(topic, sizeof(topic), "%s/%s", root, leaf);
  return p(topic, body, retain);
}
/* Newly built telemetry/discovery graphs have no users after publication.
 * Retire them before the MQTT owner can block or dispatch packet callbacks. */
static __attribute__((noinline)) int direct_json_optional(int (*p)(const char *, const char *, bool),
                        const char *root,const char *leaf,json_object *o,bool retain,service_mqtt_json_scope *scope) {
  char topic[FEATURE_TOPIC_MAX];
  mqtt_format(topic,sizeof(topic),"%s/%s",root,leaf);
  return service_mqtt_optional_send(p,topic,o,retain,scope);
}
#ifdef FEATURE_DEVICE_A1
/* Shared by virtual controls and per-A1 filter entities. Keep their parent
 * identity identical while preserving each physical filter's own state/control. */
static bool virtual_identity(const char *group, const feature_member_t *members,
                             unsigned member_count, const char *root,
                             const mqtt_account_t *account, char owner[13],
                             char *safe, size_t safe_size, char *id, size_t id_size) {
  strcpy(owner, LOCAL_ID);
  for (unsigned i = 0; i < member_count; ++i)
    if (strcmp(members[i].id, owner) < 0) strcpy(owner, members[i].id);
  if (!mqtt_format(safe, safe_size, "%s", root)) return false;
  for (char *p = safe; *p; ++p) if (*p == ':') *p = '_';
  if (account->mode) {
    const char *leaf = strrchr(root, '/');
    if (!mqtt_hex(safe, safe_size, leaf ? leaf + 1 : root)) return false;
    return mqtt_format(id, id_size, "v2_%s_%s_%s", account->key,
                       group[0] ? "group" : owner, safe);
  }
  return mqtt_format(id, id_size, "%s", root);
}
static const char *virtual_device_name(const char *group,
                                       const a1_config_t *device) {
  return group[0] ? group : (device->name[0] ? device->name : "Phicomm A1");
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
  bool ok = virtual_identity(c->group,c->members,c->member_count, root, &account, owner, safe, sizeof(safe), id, id_size) &&
            mqtt_format(name, name_size, "%s", virtual_device_name(c->group, &device));
  *grouped = c->group[0] || c->member_count;
  free(c);
  return ok;
}
static bool discovery_virtual_step(int (*publish)(const char *, const char *, bool),
                              const discovery_layout_t *c, const char *root,
                              bool remove, unsigned *cursor) {
  const char *keys[] = {"fan",           "light",       "child_lock",
                        "led_always_on", "temperature", "humidity",
                        "pm25",          "formaldehyde", "co2", "scd40_temperature", "scd40_humidity"};
  const char *components[] = {"fan",    "light",  "switch", "switch",
                              "sensor", "sensor", "sensor", "sensor", "sensor", "sensor", "sensor"};
  const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮",
                         "温度", "湿度", "PM2.5", "甲醛", "二氧化碳", "SCD40 温度", "SCD40 湿度"};
  const char *english[] = {"Air purifier","Light ring","Child lock","Light always on","Temperature","Humidity","PM2.5","Formaldehyde","Carbon dioxide","SCD40 temperature","SCD40 humidity"};
  const char *icons[] = {"mdi:air-purifier", "mdi:led-strip-variant", "mdi:lock",
                         "mdi:led-on", NULL, NULL, NULL, "mdi:molecule", "mdi:molecule-co2", NULL, NULL};
  /* MQTT-owner-only scratch: publishing may dispatch callbacks on this stack. */
  static mqtt_account_t account;
  account = mqtt_snapshot();
  /* The MQTT owner publishes discovery; keep the device snapshot off its stack. */
  static a1_config_t device_config;
  a1_config_get(&device_config);
  const char *device_name = virtual_device_name(c->group, &device_config);
  char owner[13], device_id[FEATURE_TOPIC_MAX];
  static char safe[FEATURE_TOPIC_MAX];
  if (!virtual_identity(c->group,c->members,c->member_count, root, &account, owner, safe, sizeof(safe),
                        device_id, sizeof(device_id))) return false;
  unsigned sets = c->peer_count > 1 ? c->peer_count + 1 : 1;
  for (unsigned set = 0; set < sets; set++)
    for (unsigned i = 0; i < 11; i++) {
      unsigned position=set*11+i;
      if(position<*cursor)continue;
      if (set && i < 4) { *cursor=position+1;continue; }
      static char uid[FEATURE_TOPIC_MAX], path[FEATURE_TOPIC_MAX];
      const char *sid = set ? c->peer_ids[set - 1] : "average";
      mqtt_format(uid, sizeof(uid), "bound_%s_%s_%s", safe, i < 4 ? "device" : sid,
               keys[i]);
      mqtt_format(path, sizeof(path), "homeassistant/%s/%s/config", components[i],
               uid);
      if (account.mode) {
        char entity[240];
        if (!mqtt_format(entity,sizeof(entity),"bound_%s_%s_%s",safe,i<4?"device":sid,keys[i])) return false;
        strcpy(account.mac,owner);
        if (!mqtt_identity(&account,entity,uid,sizeof(uid)) || !mqtt_ha(&account,components[i],entity,path,sizeof(path))) return false;
      }
      if (remove || i >= 4) {
        /* Retire every old proxy sensor, including optional SCD40 entities.
         * Keep the resumable cursor and owner send budget during migration. */
        if(publish(path,"",true))return false;
        *cursor=position+1;return *cursor==sets*11;
      }
      service_mqtt_json_scope scope;
      if(!service_mqtt_json_begin(&scope,ph_mqtt_owner_thread())) {
        discovery_resource_wait();return false;
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
        int rc=service_mqtt_json_error(&scope,PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        if(rc==PH_MQTT_MORE)discovery_resource_wait();else publish_error=rc;
        return false;
      }
      json_build_array_add(ids, json_object_new_string(device_id));
      json_build_object_add(device, "identifiers", ids);
      js(device, "name", device_name);
      js(device, "manufacturer", "Phicomm");
      js(device, "model", "A1 Air Purifier (EMW3080B)");
      json_build_object_add(o, "device", device);
      js(o, "unique_id", uid);
      json_build_object_add(o,"retain",json_object_new_boolean(false));
      js(o, "name", ph_i18n(device_language_get(),names[i],english[i]));
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
      int rc=service_mqtt_optional_send(publish,path,o,true,&scope);
      if(rc) {
        if(rc==PH_MQTT_MORE && !publish_error)discovery_resource_wait();
        else publish_error=rc;
        return false;
      }
      mqtt_discovery_deferred=false;
      *cursor=position+1;return *cursor==sets*11;
    }
  return true;
}
/* Cleanup is finite and explicit; ordinary owner turns use only step(). */
static void discovery_virtual(int (*publish)(const char *,const char *,bool),
                              const discovery_layout_t *c,const char *root,bool remove) {
  unsigned cursor=0;
  for(;;) {
    unsigned prior=cursor;
    if(discovery_virtual_step(publish,c,root,remove,&cursor))return;
    if(publish_error || cursor==prior)return;
  }
}
#endif
static void virtual_service(int (*publish)(const char *, const char *, bool)) {
  uint32_t now = now_ms();
#ifdef FEATURE_DEVICE_A1
  if(mqtt_capture_deferred && (uint32_t)(now-mqtt_discovery_retry_at)<1000u)return;
  mqtt_capture_deferred=false;
  unsigned snapshot_token=service_json_claim();if(!snapshot_token)return;
#endif
  feature_config_t *c = malloc(sizeof(*c));
  if (!c) {
#ifdef FEATURE_DEVICE_A1
    service_json_release(snapshot_token);
    mqtt_capture_deferred=true;discovery_resource_wait();
#endif
    return;
  }
  feature_config_get(c);
  uint32_t revision=c->revision;
#ifdef FEATURE_DEVICE_A1
  bool bound = feature_bound() || c->group[0];
  char root[FEATURE_TOPIC_MAX];
  root_for(c, 0, root);
  bool coordinator = feature_is_coordinator();
  /* A lower-MAC member can take ownership while discovery is mid-batch.
   * There is no local bootstrap work left in that role. Regaining ownership
   * is detected by was_coordinator and starts a fresh discovery cursor. */
  if (!coordinator) {
    mqtt_discovery_pending = false;mqtt_discovery_deferred=false;
    mqtt_discovery_stage = mqtt_discovery_cursor = mqtt_domain_cursor = 0;
  }
  bool discovery_needed=mqtt_discovery_pending || !mqtt_online || mqtt_revision!=revision ||
      (!was_coordinator && coordinator);
  bool discovery_wait=mqtt_discovery_deferred && (uint32_t)(now-mqtt_discovery_retry_at)<1000u;
  if(coordinator && discovery_needed && !discovery_wait) {
    service_json_release(snapshot_token);snapshot_token=0;
    if(!mqtt_discovery_pending || mqtt_discovery_revision!=revision) {
      mqtt_discovery_pending=true;mqtt_discovery_stage=0;
      mqtt_discovery_cursor=mqtt_domain_cursor=0;
      mqtt_discovery_revision=revision;
    }
    bool layout_changed=strcmp(published_root,root) || published_count!=c->peer_count ||
        (published_layout && (published_layout->member_count!=c->member_count ||
        memcmp(published_layout->members,c->members,sizeof(c->members))));
    for(unsigned i=0;i<c->peer_count && !layout_changed;i++)
      layout_changed=strcmp(published_ids[i],c->peers[i].id)!=0;
    if(mqtt_discovery_stage==0) {
      free(c);c=NULL;
      if(published_root[0] && (layout_changed || !bound) && published_layout &&
         !discovery_virtual_step(publish,published_layout,published_root,true,&mqtt_discovery_cursor)) {
        free(c);return;
      }
      mqtt_discovery_stage=1;mqtt_discovery_cursor=0;
      free(c);return;
    }
    if(mqtt_discovery_stage==1) {
      discovery_layout_t layout;discovery_layout_capture(&layout,c);
      free(c);c=NULL;
      if(bound && !discovery_virtual_step(publish,&layout,root,false,&mqtt_discovery_cursor))return;
      mqtt_discovery_stage=2;return;
    }
    if(bound && mqtt_domain_cursor<COUNT(domains)) {
      service_mqtt_json_scope scope;
      if(!service_mqtt_json_begin(&scope,ph_mqtt_owner_thread())) {
        free(c);discovery_resource_wait();return;
      }
      json_object *o=config_encode(c,domains[mqtt_domain_cursor]);
      if(o)js(o,"_source",LOCAL_ID);
      free(c);c=NULL;
      if(!o) {
        int rc=service_mqtt_json_error(&scope,PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        if(rc==PH_MQTT_MORE)discovery_resource_wait();else publish_error=rc;
        return;
      }
      char leaf[40];
      mqtt_format(leaf,sizeof(leaf),"%s/state",topic_domain(domains[mqtt_domain_cursor]));
      int rc=direct_json_optional(publish,root,leaf,o,true,&scope);
      if(!rc){++mqtt_domain_cursor;mqtt_discovery_deferred=false;}
      else if(rc==PH_MQTT_MORE && !publish_error)discovery_resource_wait();
      else publish_error=rc;
      return;
    }
    if(bound) {
      if(!published_layout)published_layout=malloc(sizeof(*published_layout));
      if(!published_layout){free(c);discovery_resource_wait();return;}
      discovery_layout_capture(published_layout,c);strcpy(published_root,root);published_count=c->peer_count;
      for(unsigned i=0;i<c->peer_count;i++)strcpy(published_ids[i],c->peers[i].id);
    } else published_root[0]=0;
    mqtt_discovery_pending=false;mqtt_discovery_deferred=false;mqtt_capture_deferred=false;
  }
  /* Periodic publishing needs only this compact configuration snapshot. The
   * large native config must not survive socket/PUBACK waits or JSON builds. */
  bool grouped=c->group[0];
  free(c);c=NULL;service_json_release(snapshot_token);
  if(bound && (uint32_t)(now-mqtt_tick)>=5000) {
    mqtt_tick=now;
    mqtt_leaf_pending|=(1u<<(grouped?14:7))-1u;
  }
  /* Only virtual control leaves remain. Sensor values travel in the existing
   * physical A1 status snapshot, without a separate producer or topic. */
  if(!bound)mqtt_leaf_pending=0;
  for(unsigned attempt=0;attempt<4 && !publish_error;attempt++) {
    unsigned selected_leaf=0,selected_work=0;
    unsigned eligible_leaves=mqtt_leaf_pending & ((1u<<(grouped?14:7))-1u);
    if(!coordinator)eligible_leaves&=~127u;
    if(bound)for(unsigned offset=0;offset<14;offset++) {
      unsigned work=(mqtt_telemetry_cursor+offset)%14;
      selected_leaf=eligible_leaves&(1u<<work);
      if(selected_leaf) {
        selected_work=work;
        mqtt_telemetry_cursor=(uint8_t)((work+1)%14);
        break;
      }
    }
    if(!selected_leaf)break;
    if(bound && selected_leaf) {
      bool on;
      unsigned speed;
      /* These snapshots are only used by the MQTT owner. */
      static a1_config_t cfg;
      a1_config_get(&cfg);
      mico_rtos_lock_mutex(&a1_lock);bool led_visible=a1_state.led_visible;
      mico_rtos_unlock_mutex(&a1_lock);
      unsigned light_brightness = cfg.brightness;
      lock();
      if (sleep_state.light_sleep && light_brightness > sleep_state.brightness)
        light_brightness = sleep_state.brightness;
      unlock();
      feature_effective_fan(cfg.fan_on, cfg.speed, &on, &speed);
      char prefix[FEATURE_TOPIC_MAX];
      bool leader = feature_is_coordinator();
      for (unsigned pass = 0; pass < (grouped ? 2u : 1u); pass++) {
        if (!pass && !leader)
          continue;
        if (pass) {
          char mac[18];
          feature_colon_mac(LOCAL_ID, mac);
          mqtt_format(prefix, sizeof(prefix), "%s/%s", root, mac);
        } else
          strcpy(prefix, root);
        char b[192];
        const char *leaves[]={"fan/state","fan/mode/state","fan/percentage/state",
            "light/state","child_lock/state","led_always_on/state","availability"};
        for(unsigned leaf=0;leaf<7;leaf++) {
          unsigned bit=1u<<(pass*7+leaf);if(!(selected_leaf&bit))continue;
          const char *body=NULL;
          switch(leaf) {
            case 0:body=on?"ON":"OFF";break;
            case 1:body=feature_fan_auto()?"auto":"manual";break;
            case 2:mqtt_format(b,sizeof(b),"%u",on?speed:0);body=b;break;
            case 3:mqtt_format(b,sizeof(b),
                "{\"state\":\"%s\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
                led_visible?"ON":"OFF",light_brightness,cfg.red,cfg.green,cfg.blue);body=b;break;
            case 4:body=cfg.child_lock?"ON":"OFF";break;
            case 5:body=cfg.led_always_on?"ON":"OFF";break;
            default:body="online";break;
          }
          if(direct_leaf(publish,prefix,leaves[leaf],body,true))break;
          mqtt_leaf_pending&=~bit;
        }
        if(publish_error)break;
      }
    }
    if(publish_error)mqtt_telemetry_cursor=(uint8_t)selected_work;
  }
#endif
  mqtt_revision = revision;
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

#ifdef A1_FRESH_AIR
#include "../A1/fresh_air.h"
bool feature_fresh_values(unsigned statistic,unsigned metrics,float values[4],float *co2) {
    feature_sample_t current[FEATURE_MAX_PEERS]; unsigned count;
    lock(); memcpy(current,samples,sizeof(current)); count=configuration.peer_count; unlock();
    uint32_t now=now_ms();
    unsigned n=feature_aggregate(current,count,now,statistic,values,NULL);
    unsigned nc=feature_co2_aggregate(current,count,now,statistic,co2);
    return (!(metrics&3)||n) && (!(metrics&4)||nc);
}
unsigned feature_primary_ceiling(void) {
    lock(); unsigned cap=configuration.linkage.enabled?configuration.linkage.max_speed:100;
    if(sleep_state.fan_sleep && cap>sleep_state.max_speed)cap=sleep_state.max_speed;
    if(configuration.linkage.enabled && automatic_night>=0 && (unsigned)automatic_night<configuration.sleep_count)
        cap=configuration.sleeps[automatic_night].max_speed;
    unlock(); return cap;
}
#include "fresh_config_mp.inc"
#endif
