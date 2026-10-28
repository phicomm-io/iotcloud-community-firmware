/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bemfa.h"
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
bool bemfa_config_values_mp(bemfa_config_t *c,const ph_value *v,bool tc1)
{
    if(!c || !v || v->kind!=PHV_MAP || !phv_keys(v,"|enabled||app_id||secret_key||topics|"))return false;
    bemfa_config_t next=*c;ph_value x;bool enabled;
    if(phv_get(v,"enabled",&x)){if(!phv_bool(&x,&enabled))return false;next.enabled=enabled;}
    if(phv_get(v,"app_id",&x)&&!phv_text(&x,next.app_id,sizeof(next.app_id)))return false;
    if(phv_get(v,"secret_key",&x)&&!phv_text(&x,next.secret_key,sizeof(next.secret_key)))return false;
    if(phv_get(v,"topics",&x)){
        unsigned n=tc1?6:2;if(x.kind!=PHV_ARRAY||x.count!=n)return false;
        for(unsigned i=0;i<n;i++){ph_value topic;if(!phv_at(&x,i,&topic)||!phv_text(&topic,next.topics[i],BEMFA_TOPIC_BYTES))return false;}
    }
    next.revision++;
    if(!bemfa_config_valid(&next,tc1))return false;
    *c=next;return true;
}
#ifdef PHICOMM_BEMFA
bool bemfa_config_set_mp(const ph_value *params,const char **code)
{
    bemfa_config_t next;bemfa_config_get(&next);
#ifdef BEMFA_TC1
    bool tc1=true;
#else
    bool tc1=false;
#endif
    if(!bemfa_config_values_mp(&next,params,tc1)){*code="invalid_params";return false;}
    bool ok=bemfa_config_save(&next);*code=ok?"accepted":"storage_error";return ok;
}
void bemfa_config_write_mp(ph_writer *w){bemfa_config_mp_fields(w);}
void bemfa_config_mp_fields(ph_writer *w)
{
    bemfa_config_t c;bemfa_config_get(&c);
    phw_bool(w,"mqtt_enabled",c.mqtt_enabled);phw_begin(w,"bemfa",false);
    phw_bool(w,"enabled",c.enabled);phw_text(w,"app_id",c.app_id);phw_bool(w,"secret_key_set",c.secret_key[0]);
    phw_begin(w,"topics",true);
#ifdef BEMFA_TC1
    unsigned n=6;
#else
    unsigned n=2;
#endif
    for(unsigned i=0;i<n;i++)phw_text(w,NULL,c.topics[i]);phw_end(w);phw_end(w);
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
