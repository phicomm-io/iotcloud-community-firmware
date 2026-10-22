#include "mico.h"
#include "../common/automation.h"
#include <assert.h>
#include <time.h>
static time_t epoch=1800000000;
uint32_t test_now;
static automation_config_t stored;
static bool fail_save,busy;
static unsigned calls;
static unsigned order[16];
time_t time(time_t *out){if(out)*out=epoch;return epoch;}
void automation_load(automation_config_t *out){*out=stored;}
bool automation_save(const automation_config_t *c){if(fail_save || c->revision!=stored.revision+1)return false;stored=*c;return true;}
json_object *service_product_capabilities(void){return json_object_new_object();}
json_object *service_product(const char *a,json_object *p,const char **code){(void)p;*code=busy?"busy":"accepted";if(!strcmp(a,"status.get")){*code="ok";return json_object_new_object();}if(!busy){assert(calls<16);order[calls++]=!strcmp(a,"system.reboot")?AUTO_REBOOT:AUTO_FAN;}return NULL;}
#include "../common/automation.c"
#ifdef FEATURE_DEVICE_A1
/* Scheduling harness checks persistence/order; product controls are exercised
 * by the A1 production-control and simulator scenarios. */
bool a1_automation_execute(const automation_action_t *a,bool button){
 (void)button;if(!a->kind)return true;if(busy)return false;
 assert(calls<16);order[calls++]=a->kind==AUTO_REBOOT?AUTO_REBOOT:AUTO_FAN;return true;
}
#endif
static json_object *request(const char *s,const char *code){json_object *q=service_parse(s,strlen(s));assert(q);json_object *r=service_command(q);json_object_put(q);assert(r);const char *got=json_object_get_string(json_object_object_get(r,"code"));if(strcmp(got,code)){fprintf(stderr,"%s expected=%s got=%s\n",s,code,json_object_to_json_string(r));abort();}return r;}
#define CHECK(s,c) do{json_object *r=request(s,c);json_object_put(r);}while(0)
static void tick(unsigned seconds){epoch+=seconds;test_now+=seconds*1000;automation_poll(true);}
#ifdef AUTOMATION_TC1
#define CONTROL "outlet.set"
#define CONTROL_PARAMS "\"on\":true"
#define KEY "0"
#define KEY_INDEX 0
#else
#define CONTROL "fan.set"
#define CONTROL_PARAMS "\"on\":true,\"speed\":50"
#define KEY "1"
#define KEY_INDEX 1
#endif
int main(void){automation_defaults(&stored);service_init();
#ifdef AUTOMATION_TC1
 for(unsigned k=0;k<AUTOMATION_KEYS;k++)for(unsigned i=0;i<34;i++)assert(stored.buttons[k][i].kind==AUTO_NONE);
 automation_button(0,1,0);automation_button(0,0,1);assert(calls==0);
#endif
#ifdef FEATURE_DEVICE_A1
 assert(stored.buttons[0][0].kind==AUTO_FAN_PANEL);
 for(unsigned s=30;s<34;s++)assert(stored.buttons[0][s].kind==AUTO_FAN && stored.buttons[0][s].value==0);
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":0,\"clicks\":30,\"action\":\"fan.panel\",\"params\":{}}}","accepted");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":0,\"clicks\":30,\"action\":\"fan.panel\",\"params\":{\"on\":true}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"once\",\"at\":1800000010,\"action\":\"fan.panel\",\"params\":{}}}","invalid_params");
#endif
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"once\",\"at\":1800000010,\"action\":\"system.reset\",\"params\":{\"full\":true}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"once\",\"at\":1800000010,\"action\":\"system.reboot\",\"params\":{}}}","accepted");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"once\",\"at\":1800000010,\"action\":\"" CONTROL "\",\"params\":{" CONTROL_PARAMS "}}}","accepted");
 assert(stored.tasks[0].id==1 && stored.tasks[1].id==2);automation_poll(false);tick(0);assert(!calls);tick(9);assert(!calls);tick(1);assert(calls==2 && order[0]==AUTO_FAN && order[1]==AUTO_REBOOT);assert(!stored.tasks[0].enabled && !stored.tasks[1].enabled);tick(1);assert(calls==2);
 epoch-=20;test_now+=1000;automation_poll(true);tick(30);assert(calls==2);
 CHECK("{\"action\":\"schedules.get\",\"params\":{\"limit\":9}}","invalid_params");
 CHECK("{\"action\":\"schedules.delete\",\"params\":{\"id\":65535}}","not_found");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":" KEY ",\"seconds\":5,\"action\":\"system.reboot\",\"params\":{}}}","invalid_params");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":" KEY ",\"clicks\":2,\"action\":\"system.reboot\",\"params\":{}}}","accepted");
 automation_button(KEY_INDEX,2,0);assert(calls==3);
 fail_save=true;CHECK("{\"action\":\"schedules.delete\",\"params\":{\"id\":1}}","storage_error");assert(stored.tasks[0].id==1);fail_save=false;
 stored.tasks[0].enabled=1;stored.tasks[0].at=epoch+5;stored.tasks[0].last=0;stored.revision++;busy=true;tick(5);assert(calls==3 && !stored.tasks[0].enabled && stored.tasks[0].state==AUTO_BUSY);busy=false;
 stored.tasks[1].enabled=1;stored.tasks[1].at=epoch+10;stored.tasks[1].last=0;epoch+=100;test_now+=1000;automation_poll(true);tick(1);assert(calls==3 && stored.tasks[1].state==AUTO_MISSED);
 for(unsigned i=0;i<AUTOMATION_TASKS;i++){stored.tasks[i].id=i+1;stored.tasks[i].enabled=0;}
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"once\",\"at\":1900000000,\"action\":\"system.reboot\",\"params\":{}}}","capacity");
#ifdef FEATURE_DEVICE_A1
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":2,\"clicks\":1,\"action\":\"light_effect.next\",\"params\":{}}}","accepted");
 assert(stored.buttons[2][0].kind==AUTO_LIGHT_EFFECT && stored.buttons[2][0].value==255);
 automation_button(2,1,0);assert(calls==4);
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":2,\"seconds\":2,\"action\":\"filter.reset\",\"params\":{}}}","accepted");
 automation_button(2,0,2);assert(calls==5);
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":3,\"clicks\":1,\"action\":\"filter.reset\",\"params\":{}}}","invalid_params");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":1,\"clicks\":2,\"action\":\"light_effect.select\",\"params\":{\"effect\":4}}}","accepted");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":1,\"clicks\":2,\"action\":\"light_effect.select\",\"params\":{\"effect\":5}}}","accepted");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":1,\"clicks\":2,\"action\":\"light_effect.select\",\"params\":{\"effect\":6}}}","invalid_params");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":0,\"clicks\":3,\"action\":\"light.set\",\"params\":{\"toggle\":true}}}","accepted");
 assert(stored.buttons[0][2].value==2 && stored.buttons[0][2].speed==255);
 automation_button(0,3,0);assert(calls==6);
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":0,\"clicks\":4,\"action\":\"light_always.set\",\"params\":{\"toggle\":true}}}","accepted");
 CHECK("{\"action\":\"buttons.set\",\"params\":{\"key\":0,\"clicks\":5,\"action\":\"child_lock.set\",\"params\":{\"toggle\":true}}}","accepted");
#endif
 /* Dense migrated key maps must remain bounded on small device heaps. */
 for(unsigned k=0;k<AUTOMATION_KEYS;k++)for(unsigned b=0;b<34;b++)stored.buttons[k][b].kind=AUTO_REBOOT;
 unsigned total=0,offset=0;
 while(offset<AUTOMATION_BUTTON_SLOTS){char query[128];snprintf(query,sizeof(query),"{\"action\":\"buttons.get\",\"params\":{\"offset\":%u}}",offset);json_object *r=request(query,"ok"),*d=json_object_object_get(r,"data"),*items=json_object_object_get(d,"items");unsigned count=json_object_array_length(items);assert(count<=8);total+=count;unsigned next=json_object_get_int(json_object_object_get(d,"next"));assert(next>offset);offset=next;json_object_put(r);}
 assert(total==AUTOMATION_BUTTON_SLOTS);
 CHECK("{\"action\":\"buttons.get\",\"params\":{\"limit\":9}}","invalid_params");
#ifdef FEATURE_DEVICE_A1
#include "countdown_scenarios.inc"
#endif
 puts("PASS automation: stable IDs, persistence before action, reboot last, no catch-up/replay, busy and bounds");
}

#include "stubs/status_wire.inc"
