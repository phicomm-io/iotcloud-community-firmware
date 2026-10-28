#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "common/service_mp.h"
static unsigned mutations,captures,caps,mqtt_calls;
static bool change_during_capture,fail_capture,caps_restricted;
static long allocation_budget=-1;
static char async_session[64];
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n){if(!allocation_budget)return NULL;if(allocation_budget>0)--allocation_budget;return __real_malloc(n);}
void __wrap_free(void *p){__real_free(p);}
extern bool device_config_recovery(void);
extern void device_config_recovery_set(bool);
static ph_buf *request(const char *action,const char *id,int full){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action",action);
 if(id)phw_text(&w,"request_id",id);phw_begin(&w,"params",false);
 if(full>=0)phw_bool(&w,"full",full);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static ph_value data_value(ph_buf *out){ph_value root,data;assert(out&&phv_parse(1,out->buf,out->len,&root)&&phv_get(&root,"data",&data));return data;}
static void code(ph_buf *out,const char *expected){ph_value root,v;assert(out&&phv_parse(1,out->buf,out->len,&root)&&phv_get(&root,"code",&v)&&phv_equal_text(&v,expected));}
static void command_code(ph_buf *request,const char *expected){ph_buf *out=service_command_mp(request->buf,request->len);code(out,expected);ph_buf_free(out);ph_buf_free(request);}
static void object(ph_writer *w){phw_begin(w,NULL,false);phw_u32(w,"value",123);phw_end(w);}
void service_product_status_mp(ph_writer *w){captures++;if(change_during_capture){change_during_capture=false;service_event("info","system","boot");}if(fail_capture){fail_capture=false;w->failed=true;return;}object(w);}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){(void)extra;caps_restricted=restricted;caps++;object(w);}
void service_product_info_mp(ph_writer *w){object(w);}
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(strcmp(action,"system.reset")&&strcmp(action,"fan.set")&&strcmp(action,"system.reboot")&&strcmp(action,"wifi.scan"))return false;
 if(!phv_keys(p,!strcmp(action,"system.reset")?"|full|":"")){*out="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
 mutations++;*out="accepted";object(w);return true;
}
bool service_product_mqtt_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(strcmp(action,"sensor.session.set"))return false;mqtt_calls++;ph_value v;
 if(!phv_keys(p,"|session|")||!phv_get(p,"session",&v)||!phv_text(&v,async_session,sizeof(async_session))){*out="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
 *out="accepted";object(w);return true;
}
bool automation_mp(const char *a,const ph_value *p,ph_writer *w,const char **out){(void)a;(void)p;(void)w;(void)out;return false;}
bool service_product_ota_start(const char *url){(void)url;return true;}
