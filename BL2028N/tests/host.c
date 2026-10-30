/* Host adapter for the actual product core, auth, meter, UDP and MQTT codec. */
#define _POSIX_C_SOURCE 200809L
#include "../src/device.h"
#include "../src/udp.h"
#include "../src/button.h"
#include "../src/ota_guard.h"
#include "../src/indicator.h"
#include "../../common/gesture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ph_device d;
static ph_auth_state auth;
static ph_udp_state udp;
static ph_settings disk;
static uint32_t clock_ms, random_state;
static unsigned writes, saves, reboots;
static bool fail_save;
static char credential[33]="secret";
static void indicator_tests(void) {
    /* Normal output, no configured broker: red stays off across both phases. */
    for(unsigned relay=0;relay<2;++relay)for(unsigned t=0;t<3000;t+=150){
        ph_indicator s=ph_indicator_state(t,true,relay,true,true,false,false,false,0);
        assert(!s.red && s.blue==(bool)relay);
    }
    ph_indicator a=ph_indicator_state(0,true,false,false,false,false,false,false,0);
    ph_indicator b=ph_indicator_state(500,true,false,false,false,false,false,false,0);
    assert(!a.red&&a.blue&&b.red&&!b.blue);
    a=ph_indicator_state(0,true,true,true,false,true,false,false,0);
    b=ph_indicator_state(150,true,true,true,false,true,false,false,0);
    assert(!a.red&&a.blue&&b.red&&b.blue);
    a=ph_indicator_state(0,true,false,true,true,true,false,false,0);
    b=ph_indicator_state(1000,true,false,true,true,true,false,false,0);
    assert(!a.red&&!a.blue&&b.red&&!b.blue);
    a=ph_indicator_state(1000,true,true,true,true,true,true,false,0);
    assert(!a.red&&a.blue);
    a=ph_indicator_state(500,false,true,false,false,true,false,false,0);
    assert(!a.red&&!a.blue);
    /* The actual TC1 helper is the cadence reference. Non-aligned press times
     * and RTOS wrap must still give exactly one 160 ms pulse each second. */
    const uint32_t origins[]={37,1250,UINT32_MAX-3000};
    for(unsigned i=0;i<sizeof(origins)/sizeof(*origins);++i)for(unsigned enabled=0;enabled<2;++enabled){
        key_gesture_t gesture={0};gesture.stable=gesture.candidate=true;gesture.pressed_at=origins[i];
        unsigned pulses=0,on_samples=0;bool previous=false;
        for(uint32_t held=0;held<13000;++held){
            uint32_t t=origins[i]+held;
            ph_indicator s=ph_indicator_state(t,enabled,true,false,false,true,false,true,held);
            bool on=s.red||s.blue;
            assert(on==key_hold_feedback(&gesture,t));
            if(on){
                assert(s.red==(held>=5000u));
                assert(s.blue==(held<5000u||held>=10000u));
                ++on_samples;
            }
            if(on&&!previous)++pulses;
            previous=on;
        }
        assert(pulses==12&&on_samples==12*160);
    }
    /* Exact pulse/threshold edges and immediate return to the output state. */
    const uint32_t edges[]={999,1000,1159,1160,4999,5000,5159,5160,9999,10000,10159,10160};
    const unsigned masks[]={0,2,2,0,0,1,1,0,0,3,3,0};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);++i){
        a=ph_indicator_state(17+edges[i],false,true,true,true,true,false,true,edges[i]);
        assert(((unsigned)a.red|((unsigned)a.blue<<1))==masks[i]);
    }
    for(unsigned relay=0;relay<2;++relay){
        a=ph_indicator_state(1234,true,relay,true,true,false,false,false,10000);
        assert(!a.red&&a.blue==(bool)relay);
    }
    a=ph_indicator_state(1234,false,true,true,true,false,false,false,10000);assert(!a.red&&!a.blue);
}
static void ota_tests(void) {
    unsigned char data[128]={'R','B','L',0};ph_ota_guard g={0};
    assert(ph_ota_chunk(&g,200,128,data,2,4096));assert(!ph_ota_complete(&g));
    assert(ph_ota_chunk(&g,200,128,data+2,126,4096));assert(ph_ota_complete(&g));
    g=(ph_ota_guard){0};assert(ph_ota_chunk(&g,200,128,data,100,4096));assert(!ph_ota_complete(&g));
    g.failed=true;assert(!ph_ota_complete(&g));assert(!ph_ota_chunk(&g,200,128,data+100,28,4096));
    g=(ph_ota_guard){0};assert(!ph_ota_chunk(&g,404,128,data,128,4096));assert(!ph_ota_complete(&g));
    g=(ph_ota_guard){0};assert(!ph_ota_chunk(&g,200,128,data,128,127));
    g=(ph_ota_guard){0};assert(!ph_ota_chunk(&g,200,0,data,128,4096));
    g=(ph_ota_guard){0};assert(!ph_ota_chunk(&g,200,128,(unsigned char*)"bad!",4,4096));
    g=(ph_ota_guard){0};assert(ph_ota_chunk(&g,200,128,data,100,4096));assert(!ph_ota_chunk(&g,200,129,data+100,28,4096));
    g=(ph_ota_guard){0};assert(ph_ota_chunk(&g,200,128,data,128,128));assert(!ph_ota_chunk(&g,200,128,data,1,128));
}
static void button_tests(void) {
    const uint32_t holds[]={100,999,1000,4999,5000,9999,10000,20000};
    const ph_button_event expected[]={PH_BUTTON_CLICK,PH_BUTTON_CLICK,PH_BUTTON_NONE,PH_BUTTON_NONE,PH_BUTTON_WIFI,PH_BUTTON_WIFI,PH_BUTTON_RESET,PH_BUTTON_RESET};
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned i=0;i<sizeof(holds)/sizeof(*holds);++i){
        uint32_t t=wrap?UINT32_MAX-3000:100;ph_button b={0};
        assert(ph_button_tick(&b,true,t)==PH_BUTTON_NONE);
        assert(ph_button_tick(&b,true,t+40)==PH_BUTTON_NONE);
        for(uint32_t n=100;n<holds[i];n+=100)assert(ph_button_tick(&b,true,t+n)==PH_BUTTON_NONE);
        assert(ph_button_tick(&b,false,t+holds[i])==PH_BUTTON_NONE);
        assert(ph_button_tick(&b,false,t+holds[i]+39)==PH_BUTTON_NONE);
        assert(ph_button_tick(&b,false,t+holds[i]+40)==expected[i]);
        assert(ph_button_tick(&b,false,t+holds[i]+100)==PH_BUTTON_NONE);
    }
    ph_button b={0};assert(!ph_button_tick(&b,true,100));assert(!ph_button_tick(&b,false,110));assert(!ph_button_tick(&b,false,160));
}
static uint32_t tick(void *p){(void)p;return clock_ms;}
static bool persist(void *p,const ph_settings *s){(void)p;if(fail_save)return false;disk=*s;++saves;return true;}
static void relay(void *p,bool on){(void)p;(void)on;++writes;}
static void reboot(void *p){(void)p;++reboots;}
static bool password(void *p,char out[33]){(void)p;strcpy(out,credential);return true;}
static bool random_bytes(void *p,uint8_t *out,size_t n){(void)p;while(n--){random_state=random_state*1664525+1013904223;*out++=(uint8_t)(random_state>>24);}return true;}
static bool auth_save(void *p,const ph_auth_store *s){ph_settings next=d.settings;next.auth=*s;next.energy_kwh=d.meter.energy_kwh;if(!persist(p,&next))return false;d.settings=next;return true;}
static void reset(bool reload){
    ph_settings s;if(reload)s=disk;else{ph_settings_default(&s);disk=s;}
    ph_device_ops ops={NULL,tick,persist,relay,reboot,NULL,NULL,NULL,NULL};
    ph_status_cache_release(&d);
    assert(ph_device_init(&d,&ops,&s,"aabbccddeeff","host-test",123));
    ph_auth_ops ao={NULL,tick,random_bytes,NULL,NULL,password,auth_save};
    assert(ph_auth_init(&auth,&ao,&d.settings.auth));memset(&udp,0,sizeof(udp));
}
static size_t unhex(char *text,unsigned char *out,size_t cap){
    size_t n=strcspn(text,"\r\n");assert(!(n&1)&&n/2<=cap);
    for(size_t i=0;i<n;i+=2){unsigned x;assert(sscanf(text+i,"%2x",&x)==1);out[i/2]=(unsigned char)x;}return n/2;
}
static void hex(const void *data,size_t n){const unsigned char *p=data;while(n--)printf("%02x",*p++);puts("");}
static size_t stats(void *out){
    ph_writer w;struct printbuf b;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,out,PH_OUTPUT_MAX));phw_begin(&w,NULL,false);
    phw_u32(&w,"writes",writes);phw_u32(&w,"saves",saves);phw_u32(&w,"reboots",reboots);phw_bool(&w,"relay",d.relay);phw_end(&w);return phw_finish_fixed(&w);
}
int main(void){
    button_tests();ota_tests();indicator_tests();random_state=7;clock_ms=10000;reset(false);
    char *line=NULL;size_t line_cap=0;unsigned char in[16384],out[PH_OUTPUT_MAX];
    while(getline(&line,&line_cap,stdin)>0){size_t n=0;char *s=line+2;
        if(line[0]=='R'){clock_ms=10000;fail_save=false;writes=saves=reboots=0;strcpy(credential,"secret");reset(false);n=stats(out);}
        else if(line[0]=='X'){reset(true);n=stats(out);}
        else if(line[0]=='T'){clock_ms=(uint32_t)strtoul(s,NULL,10);n=stats(out);}
        else if(line[0]=='F'){fail_save=atoi(s)!=0;n=stats(out);}
        else if(line[0]=='S')n=stats(out);
        else if(line[0]=='L'){ph_local_toggle(&d);n=stats(out);}
        else if(line[0]=='P'){ph_power_poll(&d);n=stats(out);}
        else if(line[0]=='C'){int a,b,c,e,used;assert(sscanf(s,"%d %d %d %d %n",&a,&b,&c,&e,&used)==4);size_t k=unhex(s+used,in,sizeof(in));n=ph_command(&d,in,k,a,b,c,e,out,sizeof(out));}
        else if(line[0]=='Q'){int f,used;assert(sscanf(s,"%d %n",&f,&used)==1);s+=used;s[strcspn(s,"\r\n")]=0;n=ph_query(&d,s,f,out,sizeof(out));}
        else if(line[0]=='U'){size_t k=unhex(s,in,sizeof(in));n=ph_udp_handle(&udp,&d,&auth,in,k,out,sizeof(out));}
        else if(line[0]=='M'){size_t k=unhex(s,in,sizeof(in));for(size_t i=0;i<k;++i)ph_meter_byte(&d.meter,in[i],clock_ms,d.settings.coefficients);n=ph_query(&d,"status.get",PH_WIRE_MSGPACK,out,sizeof(out));}
        else if(line[0]=='H'){char token[33]={0};s[strcspn(s,"\r\n")]=0;if(ph_auth_http_login(&auth,s,token))n=(size_t)sprintf((char*)out,"%s",token);}
        else if(line[0]=='B'||line[0]=='O'){char bearer[40];s[strcspn(s,"\r\n")]=0;snprintf(bearer,sizeof(bearer),"Bearer %s",s);ph_auth_context ctx;out[0]=line[0]=='B'?(ph_auth_http_check(&auth,bearer,&ctx)&&ph_auth_context_valid(&ctx,PH_AUTH_SCOPE_ADMIN)):ph_auth_http_logout(&auth,bearer);n=1;}
        else if(line[0]=='W'){s[strcspn(s,"\r\n")]=0;assert(strlen(s)<=32);strcpy(credential,s);out[0]=ph_auth_password_changed(&auth);n=1;}
        else if(line[0]=='V'){int f,used;assert(sscanf(s,"%d %n",&f,&used)==1);size_t k=unhex(s+used,in,sizeof(in));ph_value v;out[0]=phv_parse(f,in,k,&v);n=1;}
        else abort();
        hex(out,n);fflush(stdout);
    }
    ph_status_cache_release(&d);free(line);return 0;
}
