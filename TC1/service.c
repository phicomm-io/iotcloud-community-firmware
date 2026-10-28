#define AUTOMATION_TC1
#include "main.h"
#include "http_server/auth.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/signed_adapter.h"
#include "ota_server/ota_server.h"
#include <stdio.h>
#include "../common/i18n.h"
#include "../common/service.c"
#include "../common/device_status.h"
#include "../common/service_mp.h"
extern char str_mac[16];
static void reboot_worker(mico_thread_arg_t arg){(void)arg;mico_rtos_thread_msleep(1500);MicoSystemReboot();mico_rtos_delete_thread(NULL);}

__attribute__((noinline)) void service_product_status_mp(ph_writer *out)
{
    device_status_t snapshot;device_status_full_t full;
    if(!tc1_status_capture(&snapshot,&full)){out->failed=true;return;}
    device_status_mp(out,&snapshot,&full);
}
void service_product_info_mp(ph_writer *out)
{
    phw_begin(out,NULL,false);phw_text(out,"model","tc1");
    phw_text_n(out,"device_id",str_mac,strnlen(str_mac,12));phw_text(out,"version",VERSION);phw_end(out);
}
void service_product_capabilities_mp(ph_writer *out,const char *const *extra,bool restricted)
{
    static const char *const actions[]={"time.get","time.set","power.get","status.get","capabilities.get","config.get","logs.get","schedules.get","schedules.put","schedules.delete","schedules.enable","buttons.get","buttons.set","config.set","wifi.scan","wifi.scan.get","ota.start","system.reset","system.reboot","outlet.set","indicator.set","child_lock.set",NULL};
    phw_begin(out,NULL,false);phw_text(out,"model","tc1");phw_text(out,"mqtt_protocol","tmcp");
    phw_int(out,"mqtt_protocol_version",3);phw_int(out,"protocol",3);
    phw_begin(out,"actions",true);
    if(restricted){phw_text(out,NULL,"info.get");phw_text(out,NULL,"status.get");phw_text(out,NULL,"capabilities.get");}
    else for(unsigned i=0;actions[i];i++)phw_text(out,NULL,actions[i]);
    if(extra)for(unsigned i=0;extra[i];i++)phw_text(out,NULL,extra[i]);phw_end(out);
    phw_begin(out,"status_views",true);phw_text(out,NULL,"full");phw_end(out);
    phw_text(out,"device_language","zh-CN,en");phw_int(out,"config_backup",1);
    phw_bool(out,"config_recovery",device_config_recovery());phw_bool(out,"dual_mqtt",true);phw_end(out);
}
#include "service_mp.inc"
#include "../common/automation.c"
void automation_load(automation_config_t *out){mico_rtos_suspend_all_thread();*out=user_config->automation;mico_rtos_resume_all_thread();}
bool automation_save(const automation_config_t *next){if(!tc1_config_reserve())return false;automation_config_t *old=malloc(sizeof(*old));if(!old){tc1_config_release();return false;}*old=user_config->automation;bool ok=next->revision==old->revision+1;if(ok){user_config->automation=*next;ok=mico_system_context_update(sys_config)==kNoErr;if(!ok)user_config->automation=*old;}free(old);tc1_config_release();return ok;}
bool service_product_ota_start(const char *url){return ota_server_start((char *)url,NULL,NULL)==kNoErr;}
