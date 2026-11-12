#include "../common/automation.c"
#include <assert.h>
uint32_t mico_rtos_get_time(void){return 10000;}
static automation_config_t persisted;
static bool fail_save;
void automation_load(automation_config_t *c){*c=persisted;}
bool automation_save(const automation_config_t *c){if(fail_save)return false;persisted=*c;return true;}
#ifdef FEATURE_DEVICE_A1
bool a1_automation_execute(const automation_action_t*a,bool b){(void)a;(void)b;return true;}
#elif defined(AUTOMATION_TC1)
bool tc1_automation_execute(const automation_action_t*a,bool b){(void)a;(void)b;return true;}
bool tc1_automation_load_button(unsigned key,unsigned slot,automation_action_t*a){*a=persisted.buttons[key][slot];return true;}
bool tc1_automation_load_power_rules(ph_power_rule out[PH_POWER_RULES_MAX],unsigned *count){(void)out;*count=0;return true;}
bool tc1_automation_power_sample(uint32_t *out){(void)out;return false;}
bool tc1_automation_power_execute(const ph_power_rule *r){(void)r;return false;}
#else
bool m1_automation_execute(const automation_action_t*a,bool b){(void)a;(void)b;return true;}
#endif
void service_event(const char*a,const char*b,const char*c){(void)a;(void)b;(void)c;}
static ph_buf *request(bool invalid){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
 phw_bool(&w,"enabled",true);phw_text(&w,"trigger","weekly");phw_uint(&w,"days",127);phw_uint(&w,"minute",480);
#ifdef FEATURE_DEVICE_A1
 phw_text(&w,"action","fan.set");phw_begin(&w,"params",false);phw_bool(&w,"on",false);
#elif defined(AUTOMATION_TC1)
 phw_text(&w,"action","outlet.set");phw_begin(&w,"params",false);phw_bool(&w,"on",true);phw_uint(&w,"index",2);
#else
 phw_text(&w,"action","display.set");phw_begin(&w,"params",false);phw_uint(&w,"brightness",50);
#endif
 if(invalid)phw_uint(&w,"unrecognized",1);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static ph_buf *call(const char *action,ph_buf *body,const char **code){
 ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,body->buf,body->len,&p));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(automation_mp(action,&p,&w,code));return phw_take(&w);
}
int main(void){
 automation_defaults(&persisted);const char *code;ph_buf *p=request(false),*r=call("schedules.put",p,&code);assert(r&&!strcmp(code,"accepted"));assert(persisted.tasks[0].id==1&&persisted.revision==1);
 ph_value out,id;assert(phv_parse(PH_WIRE_MSGPACK,r->buf,r->len,&out)&&phv_get(&out,"id",&id));int64_t n;assert(phv_int(&id,&n)&&n==1);ph_buf_free(r);ph_buf_free(p);
 p=request(true);r=call("schedules.put",p,&code);assert(r&&!strcmp(code,"invalid_params"));assert(persisted.revision==1);ph_buf_free(r);ph_buf_free(p);
 p=request(false);fail_save=true;r=call("schedules.put",p,&code);assert(r&&!strcmp(code,"storage_error"));assert(persisted.revision==1);ph_buf_free(r);ph_buf_free(p);fail_save=false;
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);phw_end(&w);p=phw_take(&w);r=call("schedules.get",p,&code);assert(r&&!strcmp(code,"ok"));assert(phv_parse(PH_WIRE_MSGPACK,r->buf,r->len,&out));ph_value items;assert(phv_get(&out,"items",&items)&&items.kind==PHV_ARRAY&&items.count==1);ph_buf_free(r);ph_buf_free(p);
 automation_button(0,1,0);return 0;
}
