/* Fake peripherals only. Real service envelope and MQTT worker are linked. */
#include "../../common/service.h"
json_object *automation_command(const char *a,json_object *p,const char **code,bool *handled){(void)a;(void)p;(void)code;*handled=false;return NULL;}
json_object *service_product_capabilities(void){json_object *o=json_object_new_object();json_object_object_add(o,"protocol",json_object_new_int(2));return o;}
json_object *service_product(const char *a,json_object *p,const char **code){
 *code="unsupported";
 if(!strcmp(a,"status.get") || !strcmp(a,"info.get")){json_object *o=json_object_new_object();
#ifdef HOST_A1
 json_object_object_add(o,"model",json_object_new_string("a1"));
#elif defined(HOST_M1)
 json_object_object_add(o,"model",json_object_new_string("m1"));
#else
 json_object_object_add(o,"model",json_object_new_string("tc1"));
#endif
json_object_object_add(o,"ip",json_object_new_string("127.0.0.1"));json_object_object_add(o,"version",json_object_new_string("host-test"));json_object_object_add(o,"sensor_valid",json_object_new_boolean(true));json_object_object_add(o,"config_dirty",json_object_new_boolean(false));*code="ok";return o;}
#ifdef HOST_M1
 if(!strcmp(a,"display.set")){json_object *v=json_object_object_get(p,"brightness");if(v && json_object_is_type(v,json_type_int)){int n=json_object_get_int(v);if(n>=0 && n<=100 && !(n%25)){m1_set_brightness(n/25);*code="accepted";}}}
#elif defined(HOST_A1)
 if(!strcmp(a,"fan.set")){a1_control_local("fan",p);*code="accepted";}
#else
 if(!strcmp(a,"outlet.set")){json_object *v=json_object_object_get(p,"on"),*i=json_object_object_get(p,"index");if(v && json_object_is_type(v,json_type_boolean) && i && json_object_is_type(i,json_type_int) && json_object_get_int(i)>=0 && json_object_get_int(i)<6){UserRelaySet(json_object_get_int(i),json_object_get_boolean(v));*code="accepted";}}
#endif
 return NULL;
}

#include "../../common/json_owned.h"
struct printbuf *service_product_status_wire(bool live,bool envelope,const char *id){
 (void)live;const char *code;json_object *data=service_product("status.get",NULL,&code);
 if(envelope){json_object *out=json_object_new_object();if(id)json_build_object_add(out,"request_id",json_object_new_string(id));json_build_object_add(out,"code",json_object_new_string(code));json_build_object_add(out,"message",json_object_new_string("操作成功"));json_build_object_add(out,"data",data);data=out;}
 return json_owned_to_wire(data);
}
