#include "m1.h"
#include "../common/typed_wire.h"
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
void m1_co2_source_invalidate(const char *id)
{m1_co2_source_config_t c;m1_co2_source_config_get(&c);if(c.external&&!strcmp(c.device_id,id)){mico_rtos_lock_mutex(&m1_lock);remote.co2_valid=false;mico_rtos_unlock_mutex(&m1_lock);}}
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
static void co2_source_write(ph_writer *w)
{
    m1_co2_source_config_t c;m1_co2_source_config_get(&c);uint16_t ppm,ttl;bool valid=m1_co2_source_value(&ppm,&ttl);
    phw_begin(w,NULL,false);phw_text(w,"source",c.external?"mqtt":"local");phw_text(w,"device_id",c.device_id);
    phw_text(w,"double_click",c.double_click?"switch_display":"none");phw_u32(w,"revision",c.revision);phw_bool(w,"valid",valid);if(valid)phw_u32(w,"ppm",ppm);
    mico_rtos_lock_mutex(&m1_lock);bool buttons=m1_state.mcu_button_events;mico_rtos_unlock_mutex(&m1_lock);
    phw_bool(w,"button_events_supported",buttons);phw_end(w);
}
bool m1_co2_source_mp(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
    if(!strcmp(action,"co2_source.get")){
        *code=phv_keys(p,"")?"ok":"invalid_params";if(!strcmp(*code,"ok"))co2_source_write(w);return true;
    }
    if(strcmp(action,"co2_source.set"))return false;
    *code="invalid_params";ph_value v;int64_t revision;char source[6],button[16],id[18];
    if(!phv_keys(p,"|source||device_id||double_click||revision|")||!phv_get(p,"source",&v)||!phv_text(&v,source,sizeof(source))||
       !phv_get(p,"device_id",&v)||!phv_text(&v,id,sizeof(id))||!phv_get(p,"double_click",&v)||!phv_text(&v,button,sizeof(button))||
       !phv_get(p,"revision",&v)||!phv_int(&v,&revision)||revision<0||revision>INT32_MAX)return true;
    if((strcmp(source,"local")&&strcmp(source,"mqtt"))||(strcmp(button,"none")&&strcmp(button,"switch_display")))return true;
    m1_co2_source_config_t c={.magic=M1_CO2_SOURCE_MAGIC,.revision=(uint32_t)revision,.external=!strcmp(source,"mqtt"),.double_click=!strcmp(button,"switch_display")};
    if(c.external){if(!m1_display_device_id(id,c.device_id))return true;}else if(*id)return true;
    *code=m1_co2_source_config_save(&c);if(!strcmp(*code,"accepted"))co2_source_write(w);return true;
}
