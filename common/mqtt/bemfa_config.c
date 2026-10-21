/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bemfa.h"
#include "../json_build.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
void bemfa_defaults(bemfa_config_t *c, const char *model, const char *mac) {
    memset(c, 0, sizeof(*c)); c->magic=BEMFA_MAGIC; c->version=1; c->mqtt_enabled=1;
    if (!mac) return;
    unsigned n=!strcmp(model,"tc1")?6:2;
    for (unsigned i=0;i<n;i++) snprintf(c->topics[i],BEMFA_TOPIC_BYTES,"phicomm%s%s%u%s",model,mac,i+1,n==6?"001":i?"002":"003");
}
static bool topic_valid(const char *s, const char *suffix) {
    size_t n=strnlen(s,BEMFA_TOPIC_BYTES);
    if (!n) return true;
    if (n>=BEMFA_TOPIC_BYTES || n<4 || strcmp(s+n-3,suffix)) return false;
    for (size_t i=0;i<n;i++) if (!isalnum((unsigned char)s[i]) && s[i]!='_') return false;
    return true;
}
bool bemfa_config_valid(const bemfa_config_t *c, bool tc1) {
    if (c->magic!=BEMFA_MAGIC || c->version!=1 || c->enabled>1 || c->mqtt_enabled>1 ||
        !memchr(c->app_id,0,sizeof(c->app_id)) || !memchr(c->secret_key,0,sizeof(c->secret_key))) return false;
    unsigned count=tc1?6:2, active=0;
    for (unsigned i=0;i<BEMFA_TOPICS;i++) {
        if (!topic_valid(c->topics[i],tc1?"001":i?"002":"003") || (i>=count && c->topics[i][0])) return false;
        if (!c->topics[i][0]) continue;
        active++;
        for(unsigned j=0;j<i;j++) if(!strcmp(c->topics[i],c->topics[j])) return false;
    }
    return !c->enabled || (active && c->app_id[0] && c->secret_key[0]);
}
static bool text_value(struct json_object *v, char *out, size_t cap) {
    if (!v || !json_object_is_type(v,json_type_string)) return false;
    const char *s=json_object_get_string(v);size_t n=json_object_get_string_len(v);
    if (n>=cap || n!=strlen(s)) return false;
    memcpy(out,s,n+1);return true;
}
bool bemfa_config_values(bemfa_config_t *c, struct json_object *v, bool tc1) {
    if (!v || !json_object_is_type(v,json_type_object)) return false;
    json_object_object_foreach(v,k,x) {
        if (!strcmp(k,"enabled")) {
            if(!x || !json_object_is_type(x,json_type_boolean))return false;
            c->enabled=json_object_get_boolean(x);
        } else if(!strcmp(k,"app_id")) {if(!text_value(x,c->app_id,sizeof(c->app_id)))return false;}
        else if(!strcmp(k,"secret_key")) {if(!text_value(x,c->secret_key,sizeof(c->secret_key)))return false;}
        else if(!strcmp(k,"topics")) {
            unsigned n=tc1?6:2;
            if(!x || !json_object_is_type(x,json_type_array) || json_object_array_length(x)!=n)return false;
            for(unsigned i=0;i<n;i++)if(!text_value(json_object_array_get_idx(x,i),c->topics[i],BEMFA_TOPIC_BYTES))return false;
        } else return false;
    }
    c->revision++;
    return bemfa_config_valid(c,tc1);
}
#ifdef PHICOMM_BEMFA
void bemfa_config_encode(struct json_object *o) {
    bemfa_config_t c;bemfa_config_get(&c);
    json_build_object_add(o,"mqtt_enabled",json_object_new_boolean(c.mqtt_enabled));
    struct json_object *b=json_object_new_object(),*topics=json_object_new_array();
    json_build_object_add(b,"enabled",json_object_new_boolean(c.enabled));
    json_build_object_add(b,"app_id",json_object_new_string(c.app_id));
    json_build_object_add(b,"secret_key_set",json_object_new_boolean(c.secret_key[0]));
#ifdef BEMFA_TC1
    unsigned n=6;
#else
    unsigned n=2;
#endif
    for(unsigned i=0;i<n;i++)json_build_array_add(topics,json_object_new_string(c.topics[i]));
    json_build_object_add(b,"topics",topics);json_build_object_add(o,"bemfa",b);
}
#endif
unsigned bemfa_gear_speed(unsigned gear) { static const unsigned speeds[]={0,20,50,75,100}; return gear<5?speeds[gear]:0; }
unsigned bemfa_fan_gear(unsigned speed) {
    unsigned best=1,distance=101;
    for(unsigned i=1;i<=4;i++){unsigned v=bemfa_gear_speed(i);unsigned d=v>speed?v-speed:speed-v;if(d<=distance){best=i;distance=d;}}
    return best;
}
bool bemfa_parse(unsigned kind,const void *bytes,size_t n,bemfa_command_t *out) {
    if(!bytes || !out || !n || n>16 || memchr(bytes,0,n))return false;
    const char *s=bytes;memset(out,0,sizeof(*out));out->kind=kind;
    if(n==3 && !memcmp(s,"off",3))return true;
    if(n<2 || memcmp(s,"on",2))return false;
    out->on=true;if(n==2)return true;
    if(kind==BEMFA_SOCKET || n<4 || s[2]!='#')return false;
    unsigned v=0;
    for(size_t i=3;i<n;i++){if(s[i]<'0'||s[i]>'9')return false;v=v*10+s[i]-'0';if(v>100)return false;}
    if(!v || (kind==BEMFA_FAN && (n!=4 || v>4)) || kind>BEMFA_LIGHT)return false;
    out->has_value=true;out->value=kind==BEMFA_FAN?bemfa_gear_speed(v):v;return true;
}
