/* Fake peripherals only. Real native envelope, ownership and MQTT worker link.
 * Full product validation/storage matrices run in the native manifest. */
#include "../../common/service.h"
bool automation_mp(const char *a,const ph_value *p,ph_writer *w,const char **code){
 (void)a;(void)p;(void)w;(void)code;return false;
}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){
 (void)extra;(void)restricted;phw_begin(w,NULL,false);phw_int(w,"protocol",2);phw_end(w);
}
void service_product_status_mp(ph_writer *w){
 phw_begin(w,NULL,false);
#ifdef HOST_A1
 phw_text(w,"model","a1");phw_bool(w,"on",config.fan_on);phw_uint(w,"speed",config.speed);
#elif defined(HOST_M1)
 phw_text(w,"model","m1");phw_uint(w,"brightness",m1_state.brightness_requested*25);
#else
 phw_text(w,"model","tc1");phw_begin(w,"outlets",true);
 for(unsigned i=0;i<6;i++)phw_bool(w,NULL,config.socket_status[i]);phw_end(w);
#endif
 phw_text(w,"ip","127.0.0.1");phw_text(w,"version","host-test");
 phw_bool(w,"sensor_valid",true);phw_bool(w,"config_dirty",false);phw_end(w);
}
void service_product_info_mp(ph_writer *w){service_product_status_mp(w);}
void service_product_diagnostics_mp(ph_writer *w){phw_begin(w,NULL,false);phw_end(w);}
bool service_product_mp(const char *a,const ph_value *p,ph_writer *w,const char **code){
 bool handled=false;ph_value v;int64_t n;
#ifdef HOST_M1
 if(!strcmp(a,"display.set")){
  handled=true;*code="invalid_params";
  if(phv_get(p,"brightness",&v)&&phv_int(&v,&n)&&n>=0&&n<=100&&!(n%25)){
   m1_set_brightness(n/25);*code="accepted";
  }
 }
#elif defined(HOST_A1)
 if(!strcmp(a,"fan.set")){
  handled=true;*code="invalid_params";a1_control_t r={.kind=A1_CONTROL_FAN};bool valid=true;
  if(phv_get(p,"on",&v)){valid=phv_bool(&v,&r.on);r.fields|=A1_SET_ON;}
  if(phv_get(p,"speed",&v)){
   valid=valid&&phv_int(&v,&n)&&n>=0&&n<=100;
   if(valid){r.speed=n;r.fields|=A1_SET_SPEED;}
  }
  if(valid&&r.fields&&a1_control_apply(&r,false,false))*code="accepted";
 }
#else
 if(!strcmp(a,"outlet.set")){
  handled=true;*code="invalid_params";bool on;
  if(phv_get(p,"on",&v)&&phv_bool(&v,&on)&&phv_get(p,"index",&v)&&phv_int(&v,&n)&&n>=0&&n<6){
   UserRelaySet(n,on);*code="accepted";
  }
 }
#endif
 if(handled){phw_begin(w,NULL,false);phw_end(w);}return handled;
}
