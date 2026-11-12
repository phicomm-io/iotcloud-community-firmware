/* Shared typed configuration codec. Reject an entire transaction on invalid
 * input. */
#include "phicomm_features.h"
#include "feature_mp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  const char *key;
  uint16_t offset;
  uint8_t type;
  double low, high;
} mp_field_t;
/* Descriptor offsets are storage offsets, not wire integers. Fail compilation
 * rather than truncating if a future field exceeds the compact range. Numeric
 * limits remain doubles; serialization and validation precision are unchanged. */
#define mp_OFFSET(T, k) ((uint16_t)(offsetof(T, k) + 0 * sizeof(char[offsetof(T, k) <= UINT16_MAX ? 1 : -1])))
#define mp_U(T, k, lo, hi) {#k, mp_OFFSET(T, k), 1, lo, hi}
#define mp_W(T, k, lo, hi) {#k, mp_OFFSET(T, k), 2, lo, hi}
#define mp_F(T, k, lo, hi) {#k, mp_OFFSET(T, k), 3, lo, hi}
static const mp_field_t mp_linkage_fields[] = {
    mp_U(feature_linkage_t, enabled, 0, 1),
    mp_U(feature_linkage_t, hysteresis_enabled, 0, 1),
    mp_U(feature_linkage_t, mode, 0, 2),
    mp_U(feature_linkage_t, metrics, 1, 7),
    mp_U(feature_linkage_t, on_metrics, 1, 7),
    mp_U(feature_linkage_t, off_metrics, 1, 7),
    mp_U(feature_linkage_t, statistic, 0, 5),
    mp_U(feature_linkage_t, on_enabled, 0, 1),
    mp_U(feature_linkage_t, off_enabled, 0, 1),
    mp_U(feature_linkage_t, on_all, 0, 1),
    mp_U(feature_linkage_t, off_all, 0, 1),
    mp_U(feature_linkage_t, fixed_speed, 1, 100),
    mp_U(feature_linkage_t, min_speed, 0, 100),
    mp_U(feature_linkage_t, max_speed, 1, 100),
    mp_W(feature_linkage_t, debounce_seconds, 0, 3600),
    mp_W(feature_linkage_t, min_switch_seconds, 0, 3600),
    mp_F(feature_linkage_t, on_pm, 0, 5000),
    mp_F(feature_linkage_t, off_pm, 0, 5000),
    mp_F(feature_linkage_t, on_hcho, 0, 10),
    mp_F(feature_linkage_t, off_hcho, 0, 10),
    mp_F(feature_linkage_t, target_pm, 0.01, 5000),
    mp_F(feature_linkage_t, target_hcho, 0.001, 10),
    mp_F(feature_linkage_t, low_pm, 0, 5000),
    mp_F(feature_linkage_t, high_pm, 0, 5000),
    mp_F(feature_linkage_t, low_hcho, 0, 10),
    mp_F(feature_linkage_t, high_hcho, 0, 10),
    mp_F(feature_linkage_t, kp, 0, 1000),
    mp_F(feature_linkage_t, ki, 0, 100),
    mp_F(feature_linkage_t, kd, 0, 1000)};
static const mp_field_t mp_co2_fields[] = {
    mp_F(feature_co2_config_t, on_co2, 1, 65535), mp_F(feature_co2_config_t, target_co2, 1, 65535),
    mp_F(feature_co2_config_t, low_co2, 0, 65535), mp_F(feature_co2_config_t, high_co2, 1, 65535)};
static const mp_field_t mp_light_fields[] = {
    mp_U(feature_light_t, effect, 0, 5), mp_U(feature_light_t, segments, 1, 4),
    mp_U(feature_light_t, follow_fan, 0, 1),
    mp_U(feature_light_t, speed, 1, 10),
    mp_F(feature_light_t, quality_pm, .01, 5000),
    mp_F(feature_light_t, quality_hcho, .001, 10)};
static const mp_field_t mp_segment_fields[] = {
    mp_U(feature_segment_t, source, 0, 5),
    mp_U(feature_segment_t, display, 0, 2),
    mp_F(feature_segment_t, minimum, -100, 10000),
    mp_F(feature_segment_t, maximum, -100, 10000),
    mp_F(feature_segment_t, suitable_low, -100, 10000),
    mp_F(feature_segment_t, suitable_high, -100, 10000)};
static const mp_field_t mp_event_fields[] = {
    mp_U(feature_event_t, days, 1, 127), mp_U(feature_event_t, on, 0, 1),
    mp_U(feature_event_t, speed, 1, 100), mp_W(feature_event_t, minute, 0, 1439)};
static const mp_field_t mp_sleep_fields[] = {mp_U(feature_sleep_t, days, 1, 127),
                                       mp_U(feature_sleep_t, fan, 0, 1),
                                       mp_U(feature_sleep_t, light, 0, 1),
                                       mp_U(feature_sleep_t, max_speed, 1, 100),
                                       mp_U(feature_sleep_t, brightness, 0, 255),
                                       mp_U(feature_sleep_t, block_start, 0, 1),
                                       mp_W(feature_sleep_t, start, 0, 1439),
                                       mp_W(feature_sleep_t, end, 0, 1439)};
#define mp_COUNT(a) (sizeof(a) / sizeof((a)[0]))
/* Value copies are borrowed views, never owned DOM nodes. Callers decode into
 * a working configuration and commit only after all fields pass validation. */
static ph_value mp_get(ph_value o,const char *k){ph_value v={.kind=PHV_MISSING};phv_get(&o,k,&v);return v;}
static ph_value mp_at(ph_value o,unsigned i){ph_value v={.kind=PHV_MISSING};phv_at(&o,i,&v);return v;}
static bool mp_has(ph_value v){return phv_exists(&v);}
static bool mp_is(ph_value v,unsigned k){return v.kind==k;}
static bool mp_integer(ph_value v){return v.kind==PHV_UINT||v.kind==PHV_NEG;}
static int64_t cfg_mp_integer_value(ph_value v){int64_t n=0;phv_int(&v,&n);return n;}
static double mp_num(ph_value v){double n=0;if(v.kind==PHV_TRUE)return 1;if(v.kind!=PHV_FALSE)phv_number(&v,&n);return n;}
static bool mp_decode_fields(ph_value o,void *data,const mp_field_t *f,unsigned n){
 if(o.kind!=PHV_MAP)return false;
 for(unsigned i=0;i<n;i++){
  ph_value v=mp_get(o,f[i].key);if(!mp_has(v))continue;
  bool flag=v.kind==PHV_TRUE||v.kind==PHV_FALSE;
  if(flag && !(f[i].type==1&&f[i].low==0&&f[i].high==1))return false;
  if(!flag && !mp_integer(v)&&v.kind!=PHV_REAL)return false;
  double x=mp_num(v);
  if(!isfinite(x)||x<f[i].low||x>f[i].high||(f[i].type!=3&&floor(x)!=x))return false;
  unsigned char *p=(unsigned char*)data+f[i].offset;
  if(f[i].type==3)*(float*)p=x;else if(f[i].type==2)*(uint16_t*)p=x;else *p=x;
 }
 return true;
}
static bool mp_string_field(ph_value o,const char *key,char *dst,size_t n){
 ph_value v=mp_get(o,key);if(!mp_has(v))return true;
 if(v.kind!=PHV_TEXT || v.size>=n || memchr(v.text,0,v.size)||memchr(v.text,'\r',v.size)||memchr(v.text,'\n',v.size))return false;
 return phv_text(&v,dst,n);
}
static unsigned mp_length(ph_value v){return (unsigned)v.count;}
static bool mp_linkage_decode(feature_linkage_t *l, feature_co2_config_t *co2, uint8_t *fallback, ph_value o) {
    if (!mp_decode_fields(o, l, mp_linkage_fields, mp_COUNT(mp_linkage_fields)))
      return false;
    if (mp_has(mp_get(o, "metrics"))) {
      if (!mp_has(mp_get(o, "on_metrics")))
        l->on_metrics = l->metrics;
      if (!mp_has(mp_get(o, "off_metrics")))
        l->off_metrics = l->metrics;
    }
    static const mp_field_t automatic_fields[] = {{"fallback_speed", 0, 1, 0, 100}};
    if (!mp_decode_fields(o, fallback, automatic_fields, mp_COUNT(automatic_fields))) return false;
    if (!mp_decode_fields(o, co2, mp_co2_fields, mp_COUNT(mp_co2_fields))) return false;
    return (!(l->metrics & 4) || (co2->low_co2 < co2->high_co2 && co2->target_co2 > 0 && (!l->hysteresis_enabled || co2->on_co2 > co2->target_co2))) && l->min_speed <= l->max_speed && l->low_pm < l->high_pm &&
           l->low_hcho < l->high_hcho &&
           (!l->hysteresis_enabled ||
            ((!(l->metrics & 1) || l->on_pm > l->target_pm) &&
             (!(l->metrics & 2) || l->on_hcho > l->target_hcho)));
}
static bool config_decode_mp_value(feature_config_t *c, const char *d, ph_value o) {
  if (!mp_is(o, PHV_MAP))
    return false;
  if (!strcmp(d, "linkage")) {
    return mp_linkage_decode(&c->linkage,&c->co2,&c->automatic.fallback_speed,o);
  }
  if (!strcmp(d, "light")) {
    if (!mp_decode_fields(o, &c->light, mp_light_fields, mp_COUNT(mp_light_fields)))
      return false;
    ph_value a = mp_get(o, "segment");
    if (!mp_has(a) && mp_has(mp_get(o, "segments")))
      feature_light_layout(&c->light, c->light.segments);
    if (mp_has(a)) {
      if (!mp_is(a, PHV_ARRAY) ||
          mp_length(a) > 4)
        return false;
      for (unsigned i = 0; i < (unsigned)mp_length(a); i++) {
        ph_value s = mp_at(a, i);
        feature_segment_t *t = &c->light.segment[i];
        if (!mp_decode_fields(s, t, mp_segment_fields, mp_COUNT(mp_segment_fields)) ||
            t->minimum >= t->maximum || t->suitable_low > t->suitable_high)
          return false;
        ph_value colors = mp_get(s, "colors");
        if (mp_has(colors)) {
          if (!mp_is(colors, PHV_ARRAY) ||
              mp_length(colors) != 3)
            return false;
          for (unsigned j = 0; j < 3; j++) {
            ph_value rgb = mp_at(colors, j);
            if (!mp_is(rgb, PHV_ARRAY) ||
                mp_length(rgb) != 3)
              return false;
            for (unsigned k = 0; k < 3; k++) {
              ph_value v = mp_at(rgb, k);
              int x = cfg_mp_integer_value(v);
              if (!mp_integer(v) || x < 0 || x > 255)
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
    ph_value a = mp_get(o, s ? "windows" : "events");
    if (!mp_has(a) || !mp_is(a, PHV_ARRAY) ||
        mp_length(a) > (s ? 4 : 8))
      return false;
    unsigned n = mp_length(a);
    for (unsigned i = 0; i < n; i++) {
      if (s) {
        feature_sleep_t w = {127, 1, 1, 30, 0, 1, 1320, 420};
        if (!mp_decode_fields(mp_at(a, i), &w, mp_sleep_fields,
                           mp_COUNT(mp_sleep_fields)) ||
            w.start == w.end)
          return false;
        feature_auto_night_t night = {0,30};
        static const mp_field_t night_fields[] = {mp_U(feature_auto_night_t,min_speed,0,100),mp_U(feature_auto_night_t,fallback_speed,0,100)};
        if (!mp_decode_fields(mp_at(a, i), &night, night_fields, mp_COUNT(night_fields))) return false;
        if (night.min_speed > w.max_speed) return false;
        c->automatic.night[i] = night;
        c->sleeps[i] = w;
      } else {
        feature_event_t e = {127, 1, 60, 0, 480};
        if (!mp_decode_fields(mp_at(a, i), &e, mp_event_fields,
                           mp_COUNT(mp_event_fields)))
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
    if (!mp_string_field(o, "ntp_host", c->ntp_host, sizeof(c->ntp_host)) ||
        !c->ntp_host[0])
      return false;
    ph_value v = mp_get(o, "timezone_minutes");
    if (mp_has(v)) {
      int x = cfg_mp_integer_value(v);
      if (!mp_integer(v) || x < -720 || x > 840)
        return false;
      c->timezone_minutes = x;
    }
    return true;
  }
  if (!strcmp(d, "binding")) {
    const mp_field_t group_fields[] = {
        mp_U(feature_config_t, group_force_mqtt, 0, 1),
        mp_U(feature_config_t, group_recovery, 0, 1),
        mp_W(feature_config_t, group_timeout_ms, 100, 10000),
        mp_W(feature_config_t, group_recovery_seconds, 5, 3600)};
    if (!mp_decode_fields(o, c, group_fields, mp_COUNT(group_fields)))
      return false;

    if (!mp_string_field(o, "group", c->group, sizeof(c->group)) ||
        (c->group[0] && !feature_group_valid(c->group)))
      return false;
    ph_value a = mp_get(o, "peers");
    if (mp_has(a)) {
      if (!mp_is(a, PHV_ARRAY) ||
          mp_length(a) > FEATURE_MAX_PEERS)
        return false;
      feature_peer_t old[FEATURE_MAX_PEERS];
      memcpy(old, c->peers, sizeof(old));
      unsigned oldn = c->peer_count < FEATURE_MAX_PEERS ? c->peer_count : FEATURE_MAX_PEERS;
      c->peer_count = mp_length(a);
      for (unsigned i = 0; i < c->peer_count; i++) {
        ph_value v = mp_at(a, i);
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
        if (!mp_string_field(v, "mac", mac, sizeof(mac)) ||
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
        if (!mp_string_field(v, "ip", p->ip, sizeof(p->ip)) ||
            !mp_string_field(v, "group", p->group, sizeof(p->group)))
          return false;
        const mp_field_t pf[] = {mp_U(feature_peer_t, kind, 1, 3),
                              mp_U(feature_peer_t, force_mqtt, 0, 1),
                              mp_U(feature_peer_t, recovery, 0, 1),
                              mp_W(feature_peer_t, timeout_ms, 100, 10000),
                              mp_W(feature_peer_t, recovery_seconds, 5, 3600)};
        if (!mp_decode_fields(v, p, pf, mp_COUNT(pf)) ||
            (p->group[0] && !feature_group_valid(p->group)))
          return false;
        if (p->ip[0] &&
            (inet_addr(p->ip) == INADDR_NONE || inet_addr(p->ip) == 0))
          return false;
      }
    }
    a = mp_get(o, "members");
    if (mp_has(a)) {
      if (!mp_is(a, PHV_ARRAY) ||
          mp_length(a) > 8)
        return false;
      c->member_count = mp_length(a);
      for (unsigned i = 0; i < c->member_count; i++) {
        ph_value v = mp_at(a, i);
        char mac[18] = {0};
        if (!mp_string_field(v, "mac", mac, sizeof(mac)) ||
            !feature_mac(mac, c->members[i].id) ||
            !mp_string_field(v, "ip", c->members[i].ip, 16))
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

bool feature_config_decode_mp(feature_config_t *c,const char *d,const ph_value *v){return v && config_decode_mp_value(c,d,*v);}
static void encode_fields_mp(ph_writer *w,const void *data,const mp_field_t *f,unsigned n){
 for(unsigned i=0;i<n;i++){
  const unsigned char *p=(const unsigned char*)data+f[i].offset;
  if(f[i].type==3)phw_number(w,f[i].key,*(const float*)p);
  else phw_uint(w,f[i].key,f[i].type==2?*(const uint16_t*)p:*p);
 }
}
static void linkage_encode_mp(ph_writer *w,const feature_linkage_t *l,const feature_co2_config_t *co2,unsigned fallback){
 encode_fields_mp(w,l,mp_linkage_fields,mp_COUNT(mp_linkage_fields));
 encode_fields_mp(w,co2,mp_co2_fields,mp_COUNT(mp_co2_fields));
 phw_uint(w,"fallback_speed",fallback);
}
static bool feature_config_fields_mp(const feature_config_t *c,const char *d,ph_writer *w){
 if(strcmp(d,"linkage")&&strcmp(d,"light")&&strcmp(d,"schedule")&&strcmp(d,"sleep")&&strcmp(d,"time")&&strcmp(d,"binding"))return false;
 if(!strcmp(d,"linkage"))linkage_encode_mp(w,&c->linkage,&c->co2,c->automatic.fallback_speed);
 else if(!strcmp(d,"light")){
  /* speed is normalized once; do not emit duplicate map keys. */
  for(unsigned i=0;i<mp_COUNT(mp_light_fields);i++)if(strcmp(mp_light_fields[i].key,"speed"))encode_fields_mp(w,&c->light,&mp_light_fields[i],1);
  phw_uint(w,"speed",feature_light_speed(&c->light));
  phw_begin(w,"segment",true);
  for(unsigned i=0;i<4;i++){
   phw_begin(w,NULL,false);encode_fields_mp(w,&c->light.segment[i],mp_segment_fields,mp_COUNT(mp_segment_fields));
   phw_begin(w,"colors",true);
   for(unsigned j=0;j<3;j++){phw_begin(w,NULL,true);for(unsigned k=0;k<3;k++)phw_uint(w,NULL,c->light.segment[i].colors[j][k]);phw_end(w);}
   phw_end(w);phw_end(w);
  }phw_end(w);
 }else if(!strcmp(d,"schedule")||!strcmp(d,"sleep")){
  bool sleep=!strcmp(d,"sleep");phw_begin(w,sleep?"windows":"events",true);
  for(unsigned i=0;i<(sleep?c->sleep_count:c->event_count);i++){
   phw_begin(w,NULL,false);
   if(sleep){encode_fields_mp(w,&c->sleeps[i],mp_sleep_fields,mp_COUNT(mp_sleep_fields));phw_uint(w,"min_speed",c->automatic.night[i].min_speed);phw_uint(w,"fallback_speed",c->automatic.night[i].fallback_speed);}
   else encode_fields_mp(w,&c->events[i],mp_event_fields,mp_COUNT(mp_event_fields));
   phw_end(w);
  }phw_end(w);
 }else if(!strcmp(d,"time")){
  phw_text(w,"ntp_host",c->ntp_host);phw_int(w,"timezone_minutes",c->timezone_minutes);
 }else if(!strcmp(d,"binding")){
  phw_text(w,"group",c->group);phw_bool(w,"group_force_mqtt",c->group_force_mqtt);phw_bool(w,"group_recovery",c->group_recovery);
  phw_uint(w,"group_timeout_ms",c->group_timeout_ms);phw_uint(w,"group_recovery_seconds",c->group_recovery_seconds);
  phw_begin(w,"peers",true);
  for(unsigned i=0;i<c->peer_count;i++){
   const feature_peer_t *p=&c->peers[i];phw_begin(w,NULL,false);
   phw_text(w,"mac",p->id);phw_text(w,"ip",p->ip);phw_text(w,"group",p->group);phw_uint(w,"kind",p->kind);
   phw_bool(w,"confirmed",p->confirmed);phw_bool(w,"force_mqtt",p->force_mqtt);phw_bool(w,"recovery",p->recovery);
   phw_uint(w,"timeout_ms",p->timeout_ms);phw_uint(w,"recovery_seconds",p->recovery_seconds);phw_end(w);
  }phw_end(w);phw_begin(w,"members",true);
  for(unsigned i=0;i<c->member_count;i++){phw_begin(w,NULL,false);phw_text(w,"mac",c->members[i].id);phw_text(w,"ip",c->members[i].ip);phw_end(w);}
  phw_end(w);
 }
 phw_uint(w,"revision",c->revision);return !w->failed;
}

#undef mp_U
#undef mp_W
#undef mp_F
#undef mp_COUNT

bool feature_config_encode_mp(const feature_config_t *c,const char *d,ph_writer *w){
 phw_begin(w,NULL,false);bool ok=feature_config_fields_mp(c,d,w);phw_end(w);return ok&&!w->failed;
}
