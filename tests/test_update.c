#include "mico.h"
static void mico_rtos_delete_thread(void *p){(void)p;}
#include "../common/service.c"
uint32_t test_now;
static unsigned installed;
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h){(void)a;(void)p;(void)c;*h=false;return NULL;}
json_object *service_product_capabilities(void){json_object *o=json_object_new_object();json_object_object_add(o,"actions",json_object_new_array());return o;}
json_object *service_product(const char *a,json_object *p,const char **code){assert(!strcmp(a,"ota.start"));const char *url=update_text(p,"url");assert(strstr(url,"http://127.0.0.1:")&&strstr(url,"/releases/v"));installed++;*code="accepted";return json_object_new_object();}
int main(int argc,char **argv){
 assert(argc==2);service_init();unsigned v[3];assert(update_semver("0.10.0",v)&&v[1]==10);assert(!update_semver("latest",v));assert(!update_relative("/releases/v1/../../escape"));
 const char *code="ok";bool handled=false;json_object *p=json_object_new_object();string(p,"source","missing");
 json_object *r=update_command("ota.check",p,&code,&handled);assert(!strcmp(code,"invalid_params"));json_object_put(r);
 string(p,"source","cn");r=update_command("ota.check",p,&code,&handled);assert(!strcmp(code,"accepted"));json_object_put(r);assert(update_busy);
 r=update_command("ota.check",p,&code,&handled);assert(!strcmp(code,"busy"));json_object_put(r);
 update_worker(0);assert(!update_busy);if(strcmp(update_state,argv[1])){fprintf(stderr,"expected %s got %s\n",argv[1],update_state);return 1;}
 r=update_command("ota.install",p,&code,&handled);assert(installed==(!strcmp(argv[1],"available")?1u:0u));json_object_put(r);json_object_put(p);return 0;
}
