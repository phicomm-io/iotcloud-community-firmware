#include "m1.h"
#include "../common/json_build.h"
#include <math.h>

static feature_sample_t remote;
static uint32_t remote_revision, retired_boot[4];

void m1_co2_source_reset(void)
{
    mico_rtos_lock_mutex(&m1_lock);memset(&remote,0,sizeof(remote));memset(retired_boot,0,sizeof(retired_boot));remote_revision=0;mico_rtos_unlock_mutex(&m1_lock);
}
void m1_co2_source_sample(const char *id,const feature_sample_t *sample)
{
    m1_co2_source_config_t c;m1_co2_source_config_get(&c);
    if(!c.external||strcmp(c.device_id,id)||!sample->boot||!sample->sequence)return;
    mico_rtos_lock_mutex(&m1_lock);
    bool accept=true;
    for(unsigned i=0;i<4;i++)if(sample->boot==retired_boot[i])accept=false;
    if(remote_revision==c.revision&&remote.boot==sample->boot&&(int32_t)(sample->sequence-remote.sequence)<=0)accept=false;
    if(accept){
        if(remote.boot&&remote.boot!=sample->boot){memmove(retired_boot+1,retired_boot,3*sizeof(*retired_boot));retired_boot[0]=remote.boot;}
        remote=*sample;remote_revision=c.revision;
    }
    mico_rtos_unlock_mutex(&m1_lock);
}
bool m1_co2_source_value(uint16_t *ppm,uint16_t *ttl)
{
    m1_co2_source_config_t c;m1_co2_source_config_get(&c);uint32_t now=mico_rtos_get_time(),age=0;bool valid;
    *ppm=*ttl=0;
    mico_rtos_lock_mutex(&m1_lock);
    if(c.external){
        uint32_t elapsed=(uint32_t)(now-remote.co2_received_ms);
        valid=remote_revision==c.revision&&remote.co2_valid&&remote.co2_age_ms<20000&&elapsed<20000-remote.co2_age_ms&&isfinite(remote.co2)&&remote.co2>=1&&remote.co2<=65535;
        if(valid){age=remote.co2_age_ms+elapsed;*ppm=(uint16_t)(remote.co2+0.5f);}
    }else{
        const scd40_state_t *s=&m1_state.scd40;age=now-s->updated_ms;
        valid=s->ready&&s->received&&s->co2_ppm&&age<20000;if(valid)*ppm=s->co2_ppm;
    }
    if(valid)*ttl=(20000-age+999)/1000;
    mico_rtos_unlock_mutex(&m1_lock);return valid;
}
json_object *m1_co2_source_json(void)
{
    m1_co2_source_config_t c;m1_co2_source_config_get(&c);uint16_t ppm,ttl;bool valid=m1_co2_source_value(&ppm,&ttl);
    json_object *o=json_object_new_object();
    json_build_object_add(o,"source",json_object_new_string(c.external?"mqtt":"local"));
    json_build_object_add(o,"device_id",json_object_new_string(c.device_id));
    json_build_object_add(o,"double_click",json_object_new_string(c.double_click?"switch_display":"none"));
    json_build_object_add(o,"revision",json_object_new_int(c.revision));
    json_build_object_add(o,"valid",json_object_new_boolean(valid));
    if(valid)json_build_object_add(o,"ppm",json_object_new_int(ppm));
    mico_rtos_lock_mutex(&m1_lock);bool buttons=m1_state.mcu_button_events;mico_rtos_unlock_mutex(&m1_lock);
    json_build_object_add(o,"button_events_supported",json_object_new_boolean(buttons));return o;
}
json_object *m1_co2_source_set(json_object *p,const char **code)
{
    *code="invalid_params";if(!json_object_is_type(p,json_type_object))return NULL;
    const char *keys[]={"source","device_id","double_click","revision"};
    json_object_object_foreach(p,key,value){(void)value;bool known=false;for(unsigned i=0;i<4;i++)if(!strcmp(key,keys[i]))known=true;if(!known)return NULL;}
    for(unsigned i=0;i<3;i++){
        json_object *v=json_object_object_get(p,keys[i]);
        if(!json_object_is_type(v,json_type_string) || (size_t)json_object_get_string_len(v)!=strlen(json_object_get_string(v)))return NULL;
    }
    json_object *rev=json_object_object_get(p,"revision");if(!json_object_is_type(rev,json_type_int)||json_object_get_int64(rev)<0||json_object_get_int64(rev)>INT32_MAX)return NULL;
    const char *source=json_object_get_string(json_object_object_get(p,"source")),*action=json_object_get_string(json_object_object_get(p,"double_click"));
    const char *id=json_object_get_string(json_object_object_get(p,"device_id"));
    if((strcmp(source,"local")&&strcmp(source,"mqtt"))||(strcmp(action,"none")&&strcmp(action,"switch_display"))||strlen(id)>12)return NULL;
    m1_co2_source_config_t c={.magic=M1_CO2_SOURCE_MAGIC,.revision=json_object_get_int(rev),.external=!strcmp(source,"mqtt"),.double_click=!strcmp(action,"switch_display")};
    strcpy(c.device_id,id);*code=m1_co2_source_config_save(&c);return !strcmp(*code,"accepted")?m1_co2_source_json():NULL;
}
