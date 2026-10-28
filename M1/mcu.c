#include "m1.h"
#include "../common/device_status.h"
#include "../common/service_mp.h"
#include "sensor_details.h"
#include "display_source.h"
static m1_details_t details;

/* One UART owner, one bounded HTTP exchange. No socket writes from UART task. */
static uint8_t request_frame[262], response_frame[20];
static size_t request_length;
static bool exchange_active, exchange_pending, exchange_done, exchange_ok;
static bool transfer_complete;
static uint32_t transfer_until, manual_until, auto_due, auto_deadline;
static uint8_t sequence, auto_sequence, auto_op, auto_phase;
static bool auto_waiting;
/* A write reply is followed by a separate read before reporting confirmation. */
static bool alarm_need_read, alarm_request_write;
static uint8_t alarm_request_mask;
static uint32_t alarm_request_revision, alarm_due;
static uint16_t shown_ppm;
static bool shown_valid, shown_co2;
static uint32_t shown_at, shown_until;
static int manual_display_mode=-1;
static uint32_t manual_display_revision,manual_source_revision;
static bool button_seen;
static uint8_t button_sequence;
static uint32_t display_phase_at,display_config_revision,display_source_revision;
static uint16_t sent_ppm;
static bool display_phase_set;
static m1_display_value_t dual_sent[M1_DISPLAY_LANES];
static struct {
    m1_display_readback_t value;
    uint32_t revision, updated_ms, until;
    bool selected;
} dual_shown[M1_DISPLAY_LANES];
static uint8_t dual_query_slot;
static uint32_t dual_revision, dual_profile_revision;
static bool dual_selected, dual_profile_selected, dual_profile_pending, dual_profile_confirmed;
static bool dual_probe_wait;
static bool format_probe_done, format_probe_request, dual_formatted, dual_profile_formatted;
static uint32_t format_probe_due;
static m1_display_config_t uart_display_config;
/* OP23 describes saved lane caches even while the legacy profile is active.
 * Its readback is evidence only after this configuration's OP22 succeeded.
 * All callers hold m1_lock. */
static void dual_profile_forget(void)
{
    dual_profile_pending=dual_profile_confirmed=false;
    memset(dual_shown,0,sizeof(dual_shown));
}
static uint16_t be16(const uint8_t *p){return ((uint16_t)p[0]<<8)|p[1];}
static void put16(uint8_t *p,unsigned n){p[0]=n>>8;p[1]=n;}
static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static bool formatted_reply(const uint8_t p[20],unsigned slot)
{
    return !p[4] && p[5]==3 && p[6]<=1 && p[7]<=1 && p[8]==slot && slot<2 &&
        be16(p+9)<=999 && !(p[11]&~7u) && !(slot==1 && (p[11]&1u)) &&
        be16(p+12)<=M1_DISPLAY_LAMPS_MAX && p[14]<=60 && p[15]<=1 &&
        !p[16]&&!p[17]&&!p[18] && (!p[7] || (p[15] && p[6] && p[14]));
}
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
        if(cmd==0x12){m1_state.mcu_updating=true;transfer_complete=false;memset(&details,0,sizeof(details));m1_state.mcu_sensor_details=false;m1_state.mcu_alarm_schema=0;m1_state.mcu_display_schema=0;m1_state.mcu_display_format_schema=0;format_probe_done=false;m1_state.alarm_buzzer_received=false;alarm_need_read=false;dual_profile_forget();}
        if(cmd==0x70 && (p[2]==10 || p[2]==18 || p[2]==22 || p[2]==24 || p[2]==25))dual_profile_forget();
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
        m1_state.mcu_button_events=false;m1_state.mcu_display_source=false;
        m1_state.mcu_display_schema=0;
        m1_state.mcu_display_format_schema=0;format_probe_done=false;
        m1_state.mcu_alarm_schema=0;m1_state.alarm_buzzer_received=false;
        m1_state.alarm_uart_sent=false;alarm_need_read=false;alarm_due=now;
        button_seen=false;manual_display_mode=-1;display_phase_set=false;
        dual_profile_forget();memset(dual_sent,0,sizeof(dual_sent));
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
    if(p[1]==0x70 && auto_waiting && p[2]==auto_op && p[3]==auto_sequence &&
       (auto_op!=26 || !due(now,auto_deadline))) {
        auto_waiting=false;
        if(!p[4] && auto_op==0) {
            dual_profile_forget();
            m1_state.mcu_protocol=p[5];m1_state.mcu_caps=be16(p+6);
            m1_state.mcu_extended=p[5]>=2 && (be16(p+6)&0x3c00)==0x3c00;
            m1_state.mcu_tones=p[5]>=2 && (be16(p+6)&0x4000) && p[10]==1;
            details.supported=p[11]==1 && p[12]>=7 && p[13]==9;
            m1_state.mcu_sensor_details=details.supported;
            m1_state.mcu_button_events=p[14]==1&&(p[15]&3)==3;
            m1_state.mcu_display_source=p[16]==1;
            m1_state.mcu_display_schema=p[18]==2?2:0;
            m1_state.mcu_display_format_schema=0;format_probe_done=false;format_probe_due=now;
            m1_state.mcu_alarm_schema=p[17]==1?1:0;
            if(!m1_state.mcu_alarm_schema)m1_state.alarm_buzzer_received=false;
            auto_phase=1;
        } else if(!p[4] && auto_op==13) {
            shown_ppm=be16(p+13);shown_valid=p[15]!=0;shown_co2=p[16]!=0;shown_at=now;
            shown_until=now+(uint32_t)be16(p+17)*1000;
        } else if(!p[4] && auto_op==19 && p[5]==1) {
            shown_ppm=sent_ppm;shown_valid=p[13]!=0;shown_co2=p[7]!=0;shown_at=now;
            shown_until=now+(uint32_t)p[13]*1000;
        } else if(auto_op==26 && format_probe_request) {
            format_probe_done=true;format_probe_due=now+10000;
            m1_state.mcu_display_format_schema=formatted_reply(p,0)?3:0;
        } else if(auto_op==22 || auto_op==25) {
            bool reply_empty=true;
            if(auto_op==25)for(unsigned i=5;i<19;i++)if(p[i])reply_empty=false;
            dual_profile_confirmed=dual_profile_pending && !p[4] && reply_empty && !due(now,auto_deadline);
            dual_profile_pending=false;
        } else if(auto_op==26 && formatted_reply(p,dual_query_slot)) {
            unsigned slot=p[8];const m1_display_value_t *sent=&dual_sent[slot];
            bool match=dual_formatted && dual_profile_formatted && dual_profile_confirmed &&
                dual_profile_revision==dual_revision && dual_profile_selected==dual_selected &&
                p[15] && p[6]==dual_selected &&
                (sent->valid?(p[14] && p[7]==dual_selected && be16(p+9)==sent->number &&
                    p[11]==sent->format && be16(p+12)==sent->lamps):(!p[14]&&!p[7]));
            dual_shown[slot].value=(m1_display_readback_t){.number=be16(p+9),.format=p[11],
                .lamps=be16(p+12),.ttl=p[14],.showing=p[7]!=0,.confirmed=match};
            dual_shown[slot].selected=p[6]!=0;dual_shown[slot].revision=dual_revision;
            dual_shown[slot].updated_ms=now;dual_shown[slot].until=now+(uint32_t)p[14]*1000;
        } else if(!p[4] && auto_op==23 && p[5]==2 && p[6]<=1 && p[7]<=1 &&
                  p[8]==dual_query_slot && p[8]<M1_DISPLAY_LANES && be16(p+9)<=999 &&
                  (p[11]&~7u)==0 && (p[11]&3u)!=3 && p[12]<=3 && p[13]<=60 &&
                  !p[14]&&!p[15]&&!p[16]&&!p[17]&&!p[18] &&
                  (!p[7] || (p[6] && p[13]))) {
            unsigned slot=p[8];
            const m1_display_value_t *sent=&dual_sent[slot];
            bool match=!dual_formatted && !dual_profile_formatted && dual_profile_confirmed && dual_profile_revision==dual_revision &&
                dual_profile_selected==dual_selected && p[6]==dual_selected &&
                (sent->valid ? (p[13] && p[7]==dual_selected && be16(p+9)==sent->number && p[11]==sent->format && p[12]==sent->color) : (!p[13]&&!p[7]));
            dual_shown[slot].value=(m1_display_readback_t){.number=be16(p+9),.format=p[11],
                .color=p[12],.ttl=p[13],.showing=p[7]!=0,.confirmed=match};
            dual_shown[slot].selected=p[6]!=0;dual_shown[slot].revision=dual_revision;
            dual_shown[slot].updated_ms=now;dual_shown[slot].until=now+(uint32_t)p[13]*1000;
        }
        if(auto_op==16){
            bool complete=m1_details_reply(&details,p,now);
            if(complete && details.id==0)auto_phase=1;
        }
        if(!p[4] && auto_op==15)m1_state.mcu_music_present=p[5]==1;
        auto_due=now+((auto_op==13||auto_op==19||auto_op==23||auto_op==26||auto_op==15)?250:0);
        if(auto_op==0 && dual_probe_wait && m1_state.mcu_display_schema!=2)auto_due=now+10000;
        if(p[4] || ((auto_op==22 || auto_op==25) && due(now,auto_deadline)))auto_due=now+2000;
        if(auto_op==20){
            uint8_t mask;bool active;
            bool valid=m1_parse_sensor_alarm(p,&mask,&active);
            alarm_need_read=valid && alarm_request_write && mask==alarm_request_mask;
            if(valid && !alarm_request_write){
                m1_state.alarm_buzzer_received=true;m1_state.alarm_buzzer_mask=mask;
                m1_state.alarm_buzzer_active=active;m1_state.alarm_buzzer_updated_ms=now;
                m1_state.alarm_buzzer_revision=alarm_request_revision;
            }else m1_state.alarm_buzzer_received=false;
            alarm_due=now+(alarm_need_read?0:valid && mask==alarm_request_mask?5000:2000);
            auto_due=now+(alarm_need_read?0:250);
        }
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
        if(auto_op==20){alarm_need_read=false;alarm_due=now+2000;m1_state.alarm_buzzer_received=false;}
        if(auto_op==22 || auto_op==25)dual_profile_forget();
        if(auto_op==26 && format_probe_request){format_probe_done=true;format_probe_due=now+10000;m1_state.mcu_display_format_schema=0;}
        auto_waiting=false;auto_due=now+2000;
    }
    bool ready=m1_state.mcu_extended,version=m1_state.mcu_version_received;
    bool details_supported=details.supported;
    bool generic=m1_state.mcu_display_source;
    bool dual_supported=m1_state.mcu_display_schema==2;
    bool formatted_supported=m1_state.mcu_display_format_schema==3;
    bool alarm_supported=m1_state.mcu_alarm_schema==1;
    skip=skip || auto_waiting || !due(now,auto_due) || !version;
    mico_rtos_unlock_mutex(&m1_lock);
    if(skip)return;
    m1_alarm_config_t alarm;m1_alarm_get(&alarm);
    mico_rtos_lock_mutex(&m1_lock);
    bool alarm_confirmed=m1_state.alarm_buzzer_received &&
        m1_state.alarm_buzzer_revision==alarm.revision && m1_state.alarm_buzzer_mask==alarm.reserved[1];
    bool alarm_sync=alarm_supported && due(now,alarm_due) && (!alarm_confirmed ||
        (uint32_t)(now-m1_state.alarm_buzzer_updated_ms)>=5000);
    mico_rtos_unlock_mutex(&m1_lock);
    m1_co2_config_t config;m1_co2_config_get(&config);
    m1_co2_source_config_t source;m1_co2_source_config_get(&source);
    m1_display_config_t *display=&uart_display_config;m1_display_config_get(display);
    mico_rtos_lock_mutex(&m1_lock);
    dual_formatted=display->reserved==M1_DISPLAY_FORMATTED;
    if(display->configured){
        if(manual_display_revision!=display->revision||manual_source_revision!=UINT32_MAX)manual_display_mode=-1;
        if(dual_revision!=display->revision){auto_phase=1;dual_profile_forget();dual_revision=display->revision;}
    }else{
        dual_profile_forget();
        if(manual_display_revision!=config.revision||manual_source_revision!=source.revision)manual_display_mode=-1;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    uint16_t ppm,ttl;bool fresh=m1_co2_source_value(&ppm,&ttl);
    if(!display_phase_set || display_config_revision!=config.revision || display_source_revision!=source.revision){
        display_phase_at=now;display_config_revision=config.revision;
        display_source_revision=source.revision;display_phase_set=true;
    }
    if(alarm_sync){
        bool read=alarm_confirmed || (alarm_need_read && alarm_request_revision==alarm.revision &&
            alarm_request_mask==alarm.reserved[1]);
        alarm_request_write=!read;alarm_request_revision=alarm.revision;alarm_request_mask=alarm.reserved[1];
        m1_sensor_alarm_frame(f,++sequence,!read,read?0:alarm_request_mask);
        if(!read){mico_rtos_lock_mutex(&m1_lock);m1_state.alarm_buzzer_received=false;mico_rtos_unlock_mutex(&m1_lock);}
    }
    else if(!ready){extension(f,0);dual_probe_wait=false;auto_due=now+10000;}
    else if(dual_supported && (!format_probe_done ||
            (display->configured && dual_formatted && !formatted_supported && due(now,format_probe_due)))){
        /* OP00 remains frozen at schema2. Only this harmless read discovers
         * the additive formatter; an older controller is never sent24/25. */
        extension(f,26);f[4]=0;format_probe_request=true;
    }
    else if(display->configured && dual_supported && (!dual_formatted || formatted_supported)){
        if(auto_phase<1 || auto_phase>7 || (auto_phase==7&&!details_supported))auto_phase=1;
        if(auto_phase<=2){
            unsigned slot=auto_phase-1;
            m1_display_source_value(display,slot,&dual_sent[slot]);
            const m1_display_value_t *value=&dual_sent[slot];
            extension(f,dual_formatted?24:21);put16(f+4,value->number);f[6]=value->format;
            if(dual_formatted){put16(f+7,value->lamps);f[9]=value->valid?value->ttl:0;f[10]=slot;}
            else {f[7]=value->color;f[8]=value->valid?value->ttl:0;f[9]=slot;}
        }else if(auto_phase==3){
            extension(f,dual_formatted?25:22);
            mico_rtos_lock_mutex(&m1_lock);
            dual_profile_forget();
            dual_selected=manual_display_mode<0?display->selected:manual_display_mode!=0;f[4]=dual_selected;
            dual_profile_revision=display->revision;dual_profile_selected=dual_selected;dual_profile_pending=true;
            dual_profile_formatted=dual_formatted;
            mico_rtos_unlock_mutex(&m1_lock);
        }else if(auto_phase==4 || auto_phase==5){
            extension(f,dual_formatted?26:23);dual_query_slot=auto_phase-4;f[4]=dual_query_slot;
            format_probe_request=false;
        }else if(auto_phase==6)extension(f,15);
        else {extension(f,16);mico_rtos_lock_mutex(&m1_lock);m1_details_request(&details,f+4);mico_rtos_unlock_mutex(&m1_lock);}
        if(auto_phase!=7){
            unsigned next=auto_phase+1;
            if(next==6 && !(m1_state.mcu_caps&0x8000))next=7;
            if(next==7 && !details_supported)next=1;
            auto_phase=next;
        }
    }
    else if(display->configured){
        /* A scalar-capable controller cannot execute the dual profile. Keep
         * probing capabilities without selecting or rewriting a legacy lane. */
        if(dual_supported && dual_formatted){
            mico_rtos_lock_mutex(&m1_lock);auto_due=format_probe_due;mico_rtos_unlock_mutex(&m1_lock);
            return;
        }
        extension(f,0);dual_probe_wait=true;
        auto_due=now+10000;
    }
    else {
        if(auto_phase<1 || auto_phase>6 || (auto_phase==6 && !details_supported))auto_phase=1;
        if(generic){
            if(auto_phase==1){extension(f,17);m1_display_source_encode(f+4,ppm,fresh?ttl:0,config.slot,config.low,config.high);sent_ppm=ppm;}
            if(auto_phase==2){extension(f,18);f[4]=m1_display_source_selected(config.mode,manual_display_mode,now-display_phase_at,config.interval);}
            if(auto_phase==3 || auto_phase==4)extension(f,19);
        }else{
            if(auto_phase==1){extension(f,11);f[4]=2;put16(f+5,config.low);put16(f+7,config.high);}
            if(auto_phase==2){extension(f,10);f[4]=manual_display_mode<0?config.mode:manual_display_mode;f[5]=config.slot;put16(f+6,config.interval);}
            if(auto_phase==3){extension(f,12);put16(f+4,ppm);f[6]=fresh;put16(f+7,ttl);}
            if(auto_phase==4)extension(f,13);
        }
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
        if(auto_op==20){alarm_need_read=false;alarm_due=now+2000;m1_state.alarm_buzzer_received=false;}
        if(auto_op==22 || auto_op==25)dual_profile_forget();
        if(auto_op==26 && format_probe_request){format_probe_done=true;format_probe_due=now+10000;}
        auto_waiting=false;auto_due=now+2000;m1_state.uart_errors++;
        mico_rtos_unlock_mutex(&m1_lock);
    }
}

void m1_co2_capture(device_m1_co2_t *out)
{
    m1_co2_config_t c;m1_co2_config_get(&c);
    m1_co2_source_config_t source;m1_co2_source_config_get(&source);
    mico_rtos_lock_mutex(&m1_lock);
    uint32_t now=mico_rtos_get_time();
    bool valid=shown_valid && !due(now,shown_until) && (uint32_t)(now-shown_at)<5000;
    *out=(device_m1_co2_t){.revision=c.revision,.interval=c.interval,.low=c.low,.high=c.high,
        .mode=c.mode,.slot=c.slot,.ppm=shown_ppm,
        .supported=m1_state.mcu_extended && m1_state.mcu_version_received,
        .present=source.external||m1_state.scd40.serial[0]!=0,.valid=valid,.showing=shown_co2 && valid};
    mico_rtos_unlock_mutex(&m1_lock);
}
static void m1_co2_write_mp(ph_writer *w)
{
 device_m1_co2_t c;m1_co2_capture(&c);phw_begin(w,NULL,false);
#define NUM(k,v) phw_u32(w,k,v)
#define BOOL(k,v) phw_bool(w,k,v)
 BOOL("available",c.supported&&c.present);BOOL("mcu_supported",c.supported);BOOL("module_present",c.present);
 NUM("revision",c.revision);NUM("mode",c.mode);NUM("slot",c.slot);NUM("interval_seconds",c.interval);NUM("low",c.low);NUM("high",c.high);NUM("mcu_co2_ppm",c.ppm);BOOL("mcu_data_valid",c.valid);BOOL("showing_co2",c.showing);
#undef NUM
#undef BOOL
 phw_end(w);
}
static void m1_sensor_details_mp(ph_writer *,bool);
bool m1_mcu_mp(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
 bool get=!strcmp(action,"co2_display.get"),set=!strcmp(action,"co2_display.set"),details_query=!strcmp(action,"sensor_details.get");
 if(!get&&!set&&!details_query)return false;
 *code="invalid_params";
 if(!set){if(!phv_keys(p,""))goto invalid;if(get)m1_co2_write_mp(w);else m1_sensor_details_mp(w,true);*code="ok";return true;}
 const char *keys[]={"mode","slot","interval_seconds","low","high","revision"};int values[6];const int hi[]={2,1,3600,65535,65535,INT32_MAX};
 if(!phv_keys(p,"|mode||slot||interval_seconds||low||high||revision|"))goto invalid;
 for(unsigned i=0;i<6;i++){ph_value v;int64_t n;if(!phv_get(p,keys[i],&v)||!phv_int(&v,&n)||n<0||n>hi[i])goto invalid;values[i]=(int)n;}
 if(!values[2]||values[3]>values[4])goto invalid;
 m1_co2_source_config_t source;m1_co2_source_config_get(&source);
 mico_rtos_lock_mutex(&m1_lock);bool available=m1_state.mcu_extended&&m1_state.mcu_version_received&&(source.external||m1_state.scd40.serial[0]);mico_rtos_unlock_mutex(&m1_lock);
 if(!available){*code="unsupported";goto invalid;}
 m1_co2_config_t c={.magic=M1_CO2_MAGIC,.revision=values[5],.mode=values[0],.slot=values[1],.interval=values[2],.low=values[3],.high=values[4]};
 *code=m1_co2_config_save(&c);if(!strcmp(*code,"accepted"))m1_co2_write_mp(w);else{phw_begin(w,NULL,false);phw_end(w);}return true;
invalid:phw_begin(w,NULL,false);phw_end(w);return true;
}

bool m1_button_rx(const uint8_t p[20])
{
    if(p[0]!=0x23||p[19]!=0x21||p[1]!=0x71||p[2]!=1||p[3]<1||p[3]>4)return false;
    for(unsigned i=7;i<19;i++)if(p[i])return false;
    if(button_seen&&button_sequence==p[4])return true;
    button_seen=true;button_sequence=p[4];
    if(p[3]!=2)return true;
    mico_rtos_lock_mutex(&m1_lock);
    bool ringing=m1_state.wake_alarm_active,busy=m1_state.mcu_updating||m1_state.ota_active||m1_state.reboot_pending||m1_state.wifi_trial_active;
    bool supported=m1_state.mcu_button_events&&m1_state.mcu_extended;
    mico_rtos_unlock_mutex(&m1_lock);
    if(busy)return true;
    if(ringing){m1_wake_alarm_stop();return true;}
    m1_display_switch_config_t display;m1_display_switch_get(&display);
    if(display.configured){
        mico_rtos_lock_mutex(&m1_lock);bool dual=display.formatted?m1_state.mcu_display_format_schema==3:m1_state.mcu_display_schema==2;mico_rtos_unlock_mutex(&m1_lock);
        if(!supported||!dual||!display.double_click||!display.present)return true;
        bool previous=manual_display_mode>=0 && manual_display_revision==display.revision &&
            manual_source_revision==UINT32_MAX?manual_display_mode!=0:display.selected!=0;
        mico_rtos_lock_mutex(&m1_lock);
        manual_display_mode=previous?0:1;manual_display_revision=display.revision;manual_source_revision=UINT32_MAX;
        dual_profile_forget();
        mico_rtos_unlock_mutex(&m1_lock);
        auto_phase=1;auto_due=mico_rtos_get_time();return true;
    }
    m1_co2_source_config_t source;m1_co2_source_config_get(&source);
    mico_rtos_lock_mutex(&m1_lock);bool present=source.external||m1_state.scd40.serial[0];mico_rtos_unlock_mutex(&m1_lock);
    if(!supported||!source.double_click||!present)return true;
    m1_co2_config_t config;m1_co2_config_get(&config);
    bool previous=m1_display_source_selected(config.mode,manual_display_mode,mico_rtos_get_time()-display_phase_at,config.interval);
    mico_rtos_lock_mutex(&m1_lock);
    manual_display_mode=previous?0:1;manual_display_revision=config.revision;manual_source_revision=source.revision;
    mico_rtos_unlock_mutex(&m1_lock);
    auto_phase=2;auto_due=mico_rtos_get_time();return true;
}

void m1_display_readback(m1_display_readback_t out[M1_DISPLAY_LANES],bool *selected)
{
    m1_display_switch_config_t c;m1_display_switch_get(&c);uint32_t now=mico_rtos_get_time();
    mico_rtos_lock_mutex(&m1_lock);
    *selected=c.configured?(manual_display_mode>=0 && manual_display_revision==c.revision &&
        manual_source_revision==UINT32_MAX?manual_display_mode!=0:c.selected!=0):shown_co2 && shown_valid && !due(now,shown_until);
    bool profile_confirmed=dual_profile_confirmed && dual_profile_revision==c.revision &&
        dual_profile_selected==*selected && m1_state.mcu_version_received && m1_state.mcu_extended &&
        (c.formatted?m1_state.mcu_display_format_schema==3:m1_state.mcu_display_schema==2) &&
        dual_profile_formatted==c.formatted && !m1_state.mcu_updating;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        out[i]=dual_shown[i].value;
        out[i].confirmed=out[i].confirmed && profile_confirmed && c.configured && dual_shown[i].revision==c.revision &&
            dual_shown[i].selected==*selected && (uint32_t)(now-dual_shown[i].updated_ms)<5000;
        out[i].ttl=due(now,dual_shown[i].until)?0:(uint8_t)((dual_shown[i].until-now+999)/1000);
        out[i].showing=out[i].confirmed && out[i].showing && out[i].ttl;
    }
    mico_rtos_unlock_mutex(&m1_lock);
}

bool m1_music_supported(void)
{
    mico_rtos_lock_mutex(&m1_lock);
    bool supported=m1_state.mcu_version_received && m1_state.mcu_tones &&
        (m1_state.mcu_caps&0x8000) && m1_tone_version_supported(m1_state.mcu_version);
    mico_rtos_unlock_mutex(&m1_lock);return supported;
}

void m1_details_snapshot(m1_details_t *out)
{mico_rtos_lock_mutex(&m1_lock);*out=details;mico_rtos_unlock_mutex(&m1_lock);}

/* Snapshot under UART lock; encoding is owned by the network caller. */
static void m1_sensor_details_mp(ph_writer *w,bool diagnostics)
{
 m1_details_t d;uint32_t now;mico_rtos_lock_mutex(&m1_lock);d=details;now=mico_rtos_get_time();mico_rtos_unlock_mutex(&m1_lock);
 phw_begin(w,NULL,false);bool valid=m1_details_valid(&d,now);
 phw_bool(w,"available",d.supported&&valid);phw_bool(w,"supported",d.supported);
 phw_bool(w,"fresh",d.frames[0].received&&m1_details_age(&d.frames[0],now)<M1_DETAILS_STALE_MS);phw_bool(w,"healthy",d.frames[0].received&&d.frames[0].health==1);
 if(d.frames[0].received)phw_u32(w,"age_ms",m1_details_age(&d.frames[0],now));else phw_null(w,"age_ms");phw_u32(w,"stale_after_ms",M1_DETAILS_STALE_MS);
 if(valid){
  const uint8_t *raw=d.frames[0].raw;const char *groups[]={"cf1_ug_m3","atmospheric_ug_m3","particles_per_0_1_l"};const char *keys[]={"pm1","pm25","pm10","gt_0_3","gt_0_5","gt_1_0","gt_2_5","gt_5_0","gt_10_0"};phw_begin(w,"pm",false);
  for(unsigned g=0;g<3;g++){phw_begin(w,groups[g],false);unsigned count=g==2?6:3;for(unsigned i=0;i<count;i++)phw_u32(w,keys[(g==2?3:0)+i],m1_details_u16(raw+4+g*6+i*2));phw_end(w);}phw_u32(w,"error_code",raw[29]);phw_end(w);
 }
 if(diagnostics){
  const char *names[]={"pm","hcho","sht","sht_status","sht_aux","hcho_probe","pm_probe"};static const char digits[]="0123456789abcdef";
  phw_begin(w,"diagnostics",false);
  for(unsigned i=0;i<7;i++){
   const m1_sensor_frame_t *s=&d.frames[i];char hex[65];for(unsigned j=0;j<s->length;j++){hex[j*2]=digits[s->raw[j]>>4];hex[j*2+1]=digits[s->raw[j]&15];}hex[s->length*2]=0;
   phw_begin(w,names[i],false);phw_bool(w,"captured",s->received);phw_text(w,"raw_hex",hex);phw_u32(w,"generation",s->generation);phw_u32(w,"flags",s->flags);phw_u32(w,"health",s->health);
   if(s->received)phw_u32(w,"age_ms",m1_details_age(s,now));else phw_null(w,"age_ms");phw_end(w);
  }phw_end(w);
 }phw_end(w);
}
