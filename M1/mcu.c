#include "m1.h"
#include "../common/device_status.h"
#include "../common/json_build.h"
#include "sensor_details.h"
static m1_details_t details;

/* One UART owner, one bounded HTTP exchange. No socket writes from UART task. */
static uint8_t request_frame[262], response_frame[20];
static size_t request_length;
static bool exchange_active, exchange_pending, exchange_done, exchange_ok;
static bool transfer_complete;
static uint32_t transfer_until, manual_until, auto_due, auto_deadline;
static uint8_t sequence, auto_sequence, auto_op, auto_phase;
static bool auto_waiting;
static uint16_t shown_ppm;
static bool shown_valid, shown_co2;
static uint32_t shown_at, shown_until;
static uint16_t be16(const uint8_t *p){return ((uint16_t)p[0]<<8)|p[1];}
static void put16(uint8_t *p,unsigned n){p[0]=n>>8;p[1]=n;}
static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static void extension(uint8_t f[12],uint8_t op)
{memset(f,0,12);f[0]=0x23;f[1]=0x70;f[2]=op;f[3]=++sequence;f[11]=0x21;}

bool m1_mcu_exchange(const uint8_t *p,size_t n,uint8_t out[20])
{
    if(n<6 || n>262 || p[0]!=0x23)return false;
    uint8_t cmd=p[1];
    if(cmd!=0x70 && cmd!=0x12 && cmd!=0x13 && cmd!=0x16)return false;
    if(cmd!=0x13 && (n!=12 || p[11]!=0x21))return false;
    if(cmd==0x12 && ((unsigned)p[2]*256+p[3]<164))return false;
    if(cmd==0x16 && p[2]>1)return false;
    mico_rtos_lock_mutex(&m1_lock);
    bool busy=exchange_active || !m1_state.uart_ready || m1_state.ota_active ||
        m1_state.reboot_pending || m1_state.wifi_trial_active ||
        (cmd==0x70 && m1_state.mcu_updating) ||
        ((cmd==0x13 || cmd==0x16) && !m1_state.mcu_updating) ||
        (cmd==0x16 && !p[2] && !transfer_complete);
    if(!busy) {
        if(cmd==0x12){m1_state.mcu_updating=true;transfer_complete=false;memset(&details,0,sizeof(details));m1_state.mcu_sensor_details=false;}
        exchange_active=exchange_pending=true;exchange_done=exchange_ok=false;
        memcpy(request_frame,p,n);request_length=n;
        manual_until=mico_rtos_get_time()+5000;transfer_until=mico_rtos_get_time()+15000;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    if(busy)return false;
    uint32_t until=mico_rtos_get_time()+2500;
    for(;;) {
        mico_rtos_lock_mutex(&m1_lock);
        bool done=exchange_done,ok=exchange_ok;
        if(done || due(mico_rtos_get_time(),until)) {
            if(done && ok)memcpy(out,response_frame,20);
            exchange_active=exchange_pending=false;
            mico_rtos_unlock_mutex(&m1_lock);return done && ok;
        }
        mico_rtos_unlock_mutex(&m1_lock);mico_thread_msleep(10);
    }
}

bool m1_mcu_rx(const uint8_t p[20])
{
    uint32_t now=mico_rtos_get_time();
    mico_rtos_lock_mutex(&m1_lock);
    if(p[1]==0x17) {
        m1_state.mcu_extended=false;m1_state.mcu_caps=0;m1_state.mcu_version_received=false;
        m1_state.mcu_tones=false;m1_state.mcu_music_present=false;
        auto_waiting=false;auto_due=now;
        memset(&details,0,sizeof(details));m1_state.mcu_sensor_details=false;
    }
    if(p[1]==0x15 && p[2]==1 && m1_state.mcu_updating)transfer_complete=true;
    if(p[1]==0x1a)m1_state.mcu_updating=false;
    bool match=exchange_active && !exchange_pending &&
        ((request_frame[1]==0x70 && p[1]==0x70 && p[2]==request_frame[2] && p[3]==request_frame[3]) ||
         (request_frame[1]==0x12 && p[1]==0x14) ||
         (request_frame[1]==0x13 && p[1]==0x14 && p[2]==request_frame[2] && p[3]==request_frame[3]) ||
         (request_frame[1]==0x16 && request_frame[2]==1 && p[1]==0x1a));
    if(match){memcpy(response_frame,p,20);exchange_done=exchange_ok=true;}
    if(match && p[1]==0x70 && p[2]==15 && request_frame[4]==0 && !p[4])
        m1_state.mcu_music_present=p[5]==1;
    if(match && p[1]==0x70 && p[2]==15 && request_frame[4]==3 && !p[4])m1_state.mcu_music_present=true;
    if(p[1]==0x70 && auto_waiting && p[2]==auto_op && p[3]==auto_sequence) {
        auto_waiting=false;
        if(!p[4] && auto_op==0) {
            m1_state.mcu_protocol=p[5];m1_state.mcu_caps=be16(p+6);
            m1_state.mcu_extended=p[5]>=2 && (be16(p+6)&0x3c00)==0x3c00;
            m1_state.mcu_tones=p[5]>=2 && (be16(p+6)&0x4000) && p[10]==1;
            details.supported=p[11]==1 && p[12]>=7 && p[13]==9;
            m1_state.mcu_sensor_details=details.supported;
            auto_phase=1;
        } else if(!p[4] && auto_op==13) {
            shown_ppm=be16(p+13);shown_valid=p[15]!=0;shown_co2=p[16]!=0;shown_at=now;
            shown_until=now+(uint32_t)be16(p+17)*1000;
        }
        if(auto_op==16){
            bool complete=m1_details_reply(&details,p,now);
            if(complete && details.id==0)auto_phase=1;
        }
        if(!p[4] && auto_op==15)m1_state.mcu_music_present=p[5]==1;
        auto_due=now+((auto_op==13||auto_op==15)?1000:0);
        if(p[4])auto_due=now+2000;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    return match || p[1]==0x70 || p[1]==0x14 || p[1]==0x15 || p[1]==0x17 || p[1]==0x1a;
}

void m1_mcu_tick(void)
{
    uint32_t now=mico_rtos_get_time();uint8_t f[262];size_t n=0;
    mico_rtos_lock_mutex(&m1_lock);
    if(m1_state.mcu_updating && due(now,transfer_until))m1_state.mcu_updating=false;
    if(exchange_pending) {
        n=request_length;memcpy(f,request_frame,n);exchange_pending=false;
        mico_rtos_unlock_mutex(&m1_lock);
        bool ok=MicoUartSend(MICO_UART_1,f,n)==kNoErr;
        mico_rtos_lock_mutex(&m1_lock);
        if(!ok){exchange_done=true;exchange_ok=false;m1_state.uart_errors++;}
        else if(f[1]==0x16 && !f[2]) {
            memset(response_frame,0,20);response_frame[0]=0x23;response_frame[1]=0x16;response_frame[19]=0x21;
            exchange_done=exchange_ok=true; /* transmit receipt, not boot proof */
        }
        mico_rtos_unlock_mutex(&m1_lock);return;
    }
    bool skip=exchange_active || m1_state.mcu_updating || m1_state.ota_active ||
        m1_state.reboot_pending || m1_state.wifi_trial_active || !due(now,manual_until);
    if(auto_waiting && due(now,auto_deadline)){
        if(auto_op==16){m1_details_fail(&details);auto_phase=1;}
        auto_waiting=false;auto_due=now+2000;
    }
    bool ready=m1_state.mcu_extended,version=m1_state.mcu_version_received;
    bool details_supported=details.supported;
    scd40_state_t sensor=m1_state.scd40;
    skip=skip || auto_waiting || !due(now,auto_due) || !version;
    mico_rtos_unlock_mutex(&m1_lock);
    if(skip)return;
    m1_co2_config_t config;m1_co2_config_get(&config);
    if(!ready){extension(f,0);auto_due=now+10000;}
    else {
        if(auto_phase<1 || auto_phase>6 || (auto_phase==6 && !details_supported))auto_phase=1;
        if(auto_phase==1){extension(f,11);f[4]=2;put16(f+5,config.low);put16(f+7,config.high);}
        if(auto_phase==2){extension(f,10);f[4]=config.mode;f[5]=config.slot;put16(f+6,config.interval);}
        if(auto_phase==3){extension(f,12);bool fresh=sensor.ready && sensor.received && (uint32_t)(now-sensor.updated_ms)<20000 && sensor.co2_ppm;
            put16(f+4,sensor.co2_ppm);f[6]=fresh;put16(f+7,fresh?(20000-(now-sensor.updated_ms)+999)/1000:0);}
        if(auto_phase==4)extension(f,13);
        if(auto_phase==5)extension(f,15);
        if(auto_phase==6){
            extension(f,16);mico_rtos_lock_mutex(&m1_lock);
            m1_details_request(&details,f+4);mico_rtos_unlock_mutex(&m1_lock);
        }
        else {
            unsigned next=auto_phase+1;
            if(next==5 && !(m1_state.mcu_caps&0x8000))next=6;
            if(next==6 && !details_supported)next=1;
            auto_phase=next;
        }
    }
    mico_rtos_lock_mutex(&m1_lock);
    auto_op=f[2];auto_sequence=f[3];auto_waiting=true;auto_deadline=now+1000;
    mico_rtos_unlock_mutex(&m1_lock);
    if(MicoUartSend(MICO_UART_1,f,12)!=kNoErr){
        mico_rtos_lock_mutex(&m1_lock);
        if(auto_op==16){m1_details_fail(&details);auto_phase=1;}
        auto_waiting=false;auto_due=now+2000;m1_state.uart_errors++;
        mico_rtos_unlock_mutex(&m1_lock);
    }
}

void m1_co2_capture(device_m1_co2_t *out)
{
    m1_co2_config_t c;m1_co2_config_get(&c);
    mico_rtos_lock_mutex(&m1_lock);
    uint32_t now=mico_rtos_get_time();
    bool valid=shown_valid && !due(now,shown_until) && (uint32_t)(now-shown_at)<5000;
    *out=(device_m1_co2_t){.revision=c.revision,.interval=c.interval,.low=c.low,.high=c.high,
        .mode=c.mode,.slot=c.slot,.ppm=shown_ppm,
        .supported=m1_state.mcu_extended && m1_state.mcu_version_received,
        .present=m1_state.scd40.serial[0]!=0,.valid=valid,.showing=shown_co2 && valid};
    mico_rtos_unlock_mutex(&m1_lock);
}
json_object *m1_co2_json(void)
{
    device_m1_co2_t c;m1_co2_capture(&c);
    json_object *o=json_object_new_object();
#define NUM(k,v) json_build_object_add(o,k,json_object_new_int(v))
#define BOOL(k,v) json_build_object_add(o,k,json_object_new_boolean(v))
    BOOL("available",c.supported && c.present);BOOL("mcu_supported",c.supported);BOOL("module_present",c.present);
    NUM("revision",c.revision);NUM("mode",c.mode);NUM("slot",c.slot);NUM("interval_seconds",c.interval);
    NUM("low",c.low);NUM("high",c.high);NUM("mcu_co2_ppm",c.ppm);BOOL("mcu_data_valid",c.valid);BOOL("showing_co2",c.showing);
#undef NUM
#undef BOOL
    return o;
}
json_object *m1_co2_set(json_object *p,const char **code)
{
    *code="invalid_params";
    const char *keys[]={"mode","slot","interval_seconds","low","high","revision"};int values[6];
    const int hi[]={2,1,3600,65535,65535,INT32_MAX};
    json_object_object_foreach(p,key,value){(void)value;bool found=false;for(unsigned i=0;i<6;i++)if(!strcmp(key,keys[i]))found=true;if(!found)return NULL;}
    for(unsigned i=0;i<6;i++){json_object *v=json_object_object_get(p,keys[i]);if(!v || !json_object_is_type(v,json_type_int))return NULL;values[i]=json_object_get_int(v);if(values[i]<0 || values[i]>hi[i])return NULL;}
    if(!values[2] || values[3]>values[4])return NULL;
    mico_rtos_lock_mutex(&m1_lock);bool available=m1_state.mcu_extended && m1_state.mcu_version_received && m1_state.scd40.serial[0];mico_rtos_unlock_mutex(&m1_lock);
    if(!available){*code="unsupported";return NULL;}
    m1_co2_config_t c={.magic=M1_CO2_MAGIC,.revision=values[5],.mode=values[0],.slot=values[1],.interval=values[2],.low=values[3],.high=values[4]};
    *code=m1_co2_config_save(&c);return !strcmp(*code,"accepted")?m1_co2_json():NULL;
}

bool m1_music_supported(void)
{
    mico_rtos_lock_mutex(&m1_lock);
    bool supported=m1_state.mcu_version_received && m1_state.mcu_tones &&
        (m1_state.mcu_caps&0x8000) && m1_tone_version_supported(m1_state.mcu_version);
    mico_rtos_unlock_mutex(&m1_lock);return supported;
}

/* Snapshot under the UART lock; JSON allocation belongs to the network owner. */
static json_object *sensor_details_json(bool diagnostics)
{
    m1_details_t d;uint32_t now;
    mico_rtos_lock_mutex(&m1_lock);d=details;now=mico_rtos_get_time();mico_rtos_unlock_mutex(&m1_lock);
    json_object *o=json_object_new_object();
#define DB(k,v) json_build_object_add(o,k,json_object_new_boolean(v))
    bool valid=m1_details_valid(&d,now);
    DB("available",d.supported && valid);DB("supported",d.supported);
    DB("fresh",d.frames[0].received && m1_details_age(&d.frames[0],now)<M1_DETAILS_STALE_MS);
    DB("healthy",d.frames[0].received && d.frames[0].health==1);
#undef DB
    if(d.frames[0].received)json_build_object_add(o,"age_ms",json_object_new_int64(m1_details_age(&d.frames[0],now)));
    else json_object_object_add(o,"age_ms",NULL);
    json_build_object_add(o,"stale_after_ms",json_object_new_int(M1_DETAILS_STALE_MS));
    if(valid){
        json_object *pm=json_object_new_object();const uint8_t *raw=d.frames[0].raw;
        const char *groups[]={"cf1_ug_m3","atmospheric_ug_m3","particles_per_0_1_l"};
        const char *keys[]={"pm1","pm25","pm10","gt_0_3","gt_0_5","gt_1_0","gt_2_5","gt_5_0","gt_10_0"};
        for(unsigned g=0;g<3;g++){
            json_object *values=json_object_new_object();unsigned count=g==2?6:3;
            for(unsigned i=0;i<count;i++)json_build_object_add(values,keys[(g==2?3:0)+i],json_object_new_int(m1_details_u16(raw+4+g*6+i*2)));
            json_build_object_add(pm,groups[g],values);
        }
        json_build_object_add(pm,"error_code",json_object_new_int(raw[29]));json_build_object_add(o,"pm",pm);
    }
    if(!diagnostics)return o;
    json_object *diag=json_object_new_object();
    const char *names[]={"pm","hcho","sht","sht_status","sht_aux","hcho_probe","pm_probe"};
    for(unsigned i=0;i<7;i++){
        const m1_sensor_frame_t *s=&d.frames[i];json_object *v=json_object_new_object();char hex[65];
        for(unsigned j=0;j<s->length;j++)snprintf(hex+j*2,3,"%02x",s->raw[j]);hex[s->length*2]=0;
        json_build_object_add(v,"captured",json_object_new_boolean(s->received));
        json_build_object_add(v,"raw_hex",json_object_new_string(hex));
        json_build_object_add(v,"generation",json_object_new_int(s->generation));
        json_build_object_add(v,"flags",json_object_new_int(s->flags));
        json_build_object_add(v,"health",json_object_new_int(s->health));
        if(s->received)json_build_object_add(v,"age_ms",json_object_new_int64(m1_details_age(s,now)));
        else json_object_object_add(v,"age_ms",NULL);
        json_build_object_add(diag,names[i],v);
    }
    json_build_object_add(o,"diagnostics",diag);return o;
}

json_object *m1_sensor_details_json(void){return sensor_details_json(true);}
json_object *m1_particles_json(void){return sensor_details_json(false);}
