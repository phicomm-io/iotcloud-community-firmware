#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "json.h"
static unsigned launches,states,results;static bool busy;
static const char *current_state="receiving";
static bool ota_launch(const char *url) {if(busy || strncmp(url,"http://",7))return false;launches++;return true;}
static void ota_snapshot(char *state,char *error,unsigned *progress){strcpy(state,current_state);error[0]=0;*progress=50;}
static int ota_publish(const char *leaf,const char *text,bool retained){assert(strstr(text,"request_id"));if(!strcmp(leaf,"ota/state")){assert(retained);states++;}else{assert(!retained);results++;}return 0;}
#define PH_MQTT_ERROR_APP_OTA_GRAPH 11
#define PH_MQTT_ERROR_APP_OTA_ENCODING 12
static unsigned app_stage;static bool fail_graph,fail_encoding;
static int ph_mqtt_app_error(unsigned stage,int rc){assert((stage==11 || stage==12) && rc<0);app_stage=stage;return rc;}
static json_object *graph(void){return fail_graph?NULL:json_object_new_object();}
static const char *encoding(json_object *o){return fail_encoding?NULL:json_object_to_json_string(o);}
#define json_object_new_object graph
#define json_object_to_json_string encoding
#include "../../common/ota/mqtt_ota.inc"
#undef json_object_new_object
#undef json_object_to_json_string
static void command(const char *s,bool retained){ota_command("owner",s,strlen(s),retained);}
int main(void){
 const char *valid="{\"request_id\":\"r1\",\"url\":\"http://host/image\"}";
 command(valid,true);assert(!launches);
 command("{\"request_id\":\"r1\",\"url\":\"http://host/image\",\"unsigned\":true}",false);assert(!launches);
 command("{\"request_id\":\"r1\",\"url\":\"http://host/image\"}garbage",false);assert(!launches);
 command("{\"request_id\":1,\"url\":\"http://host/image\"}",false);assert(!launches);
 busy=true;command(valid,false);assert(!launches);busy=false;ota_service("owner");assert(results==1);results=0;
 command(valid,false);assert(launches==1);command(valid,false);assert(launches==1);
 ota_service("other-owner");assert(!states);
 fail_graph=true;assert(ota_service("owner")==-1 && app_stage==11 && !states && !results);fail_graph=false;
 fail_encoding=true;assert(ota_service("owner")==-1 && app_stage==12 && !states && !results);fail_encoding=false;
 ota_service("owner");assert(states==1 && !results);ota_service("owner");assert(states==1);
 current_state="failed";ota_service("owner");assert(states==2 && results==1);ota_service("owner");assert(results==1);
 ota_forget();ota_service("owner");assert(states==2);
 puts("MQTT OTA: retained, strict schema, dedup, busy, session, final publication passed");
}
