#include "mico.h"
#include "mico_system.h"
#include "json.h"
#include "../common/phicomm_features.h"
void feature_defaults(feature_config_t *f){memset(f,0,sizeof(*f));f->magic=FEATURE_MAGIC;}
#ifdef HOST_M1
#include "../M1/m1.h"
#include "../M1/config_legacy.h"
#define OLD m1_storage_v2_t
#define NEW m1_config_t
#define MIGRATE m1_migrate_v2
#define MAGIC M1_CONFIG_MAGIC
#elif defined(HOST_A1)
#include "../A1/a1.h"
#include "../A1/config_legacy.h"
#define OLD a1_storage_v2_t
#define NEW a1_config_t
#define MIGRATE a1_migrate_v2
#define MAGIC A1_CONFIG_MAGIC
#else
#include "../TC1/main.h"
#include "../TC1/config_legacy.h"
#endif
int main(void){
#if defined(HOST_M1) || defined(HOST_A1)
    OLD old={0};NEW next;feature_config_t f;
    old.base.magic=MAGIC;old.base.version=2;old.base.revision=99;
    strcpy(old.base.ssid,"old wifi");strcpy(old.base.wifi_password,"wifi secret");
    strcpy(old.base.mqtt_host,"old.example");strcpy(old.base.mqtt_user,"user@example.com");
    memset(old.base.mqtt_password,'p',63);strcpy(old.base.name,"Old name");strcpy(old.base.web_password,"old password");old.base.mqtt_port=1883;
    memset(&old.features,0xab,sizeof(old.features));old.features.magic=FEATURE_MAGIC;
#ifdef HOST_M1
    old.base.brightness=3;old.base.last_on=4;old.base.timezone_minutes=480;
#else
    old.base.fan_on=1;old.base.speed=60;old.base.red=50;old.base.green=100;old.base.blue=150;old.base.child_lock=1;
#endif
    MIGRATE(&next,&f,&old);
    assert(next.version==3 && next.mqtt_topic_mode==0 && next.revision==99);
    assert(!strcmp(next.ssid,old.base.ssid) && !strcmp(next.wifi_password,old.base.wifi_password));
    assert(!strcmp(next.mqtt_host,old.base.mqtt_host) && !strcmp(next.mqtt_user,old.base.mqtt_user));
    assert(strlen(next.mqtt_password)==63 && !strcmp(next.name,old.base.name) && !strcmp(next.web_password,old.base.web_password));
    assert(next.mqtt_port==1883 && !memcmp(&f,&old.features,sizeof(f)));
#ifdef HOST_M1
    assert(next.brightness==3 && next.last_on==4 && next.timezone_minutes==480);
#else
    assert(next.fan_on==1 && next.speed==60 && next.red==50 && next.green==100 && next.blue==150 && next.child_lock==1);
#endif
    old.base.version=1;MIGRATE(&next,&f,&old);assert(f.magic==FEATURE_MAGIC && !f.peer_count);
#else
    user_config_v10_t old={0};old.version=10;
    strcpy(old.mqtt_ip,"broker.example");strcpy(old.mqtt_user,"old@example.com");strcpy(old.mqtt_password,"old secret");old.mqtt_port=1883;
    strcpy(old.socket_names[5],"Kitchen");old.socket_status[5]=1;old.task_count=128;old.timed_tasks[127].on_use=true;old.timed_tasks[127].operation=6;
    old.power_coefficient_x100=1710;strcpy(old.web_password,"web secret");
    user_config_t next;memset(&next,0xee,sizeof(next));memcpy(&next,&old,sizeof(old));
    assert(tc1_migrate_v10(&next,sizeof(old)));
    assert(next.version==11 && next.mqtt_topic_mode==0);
    assert(!strcmp(next.mqtt_user,old.mqtt_user) && !strcmp(next.mqtt_password,old.mqtt_password));
    assert(!strcmp(next.mqtt_ip,old.mqtt_ip) && next.mqtt_port==1883 && next.task_count==128);
    assert(next.timed_tasks[127].operation==6 && next.socket_status[5]==1 && next.power_coefficient_x100==1710);
    assert(!strcmp(next.socket_names[5],"Kitchen") && !strcmp(next.web_password,"web secret"));
    assert(!tc1_migrate_v10(&next,sizeof(old))); /* no repeated migration */
#endif
    puts("Frozen old configuration migration preserves settings and selects legacy");
}
