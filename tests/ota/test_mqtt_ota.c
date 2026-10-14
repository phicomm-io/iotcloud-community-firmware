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
#include "../../common/ota/mqtt_ota.inc"
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
 ota_service("owner");assert(states==1 && !results);ota_service("owner");assert(states==1);
 current_state="failed";ota_service("owner");assert(states==2 && results==1);ota_service("owner");assert(results==1);
 ota_forget();ota_service("owner");assert(states==2);
 puts("MQTT OTA: retained, strict schema, dedup, busy, session, final publication passed");
}
