/* Actual OTA adapter, fixed binary writers and owned command lifetime. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../../common/typed_wire.h"
static unsigned launches,states,results;static bool busy,fail_encoding,fail_publish;
static const char *current_state="receiving";
static char launched_url[500];
static bool ota_launch(const char *url){
 if(busy||strncmp(url,"http://",7))return false;
 strcpy(launched_url,url);launches++;return true;
}
static void ota_snapshot(char *state,char *error,unsigned *progress){strcpy(state,current_state);error[0]=0;*progress=50;}
static int ota_publish(const char *leaf,const void *bytes,size_t length,bool retained){
 ph_value root,id;assert(phv_parse(PH_WIRE_MSGPACK,bytes,length,&root));
 assert(phv_get(&root,"request_id",&id)&&phv_equal_text(&id,"r1"));
 if(fail_publish){fail_publish=false;return -2;}
 if(!strcmp(leaf,"ota/state")){assert(retained);states++;}
 else{assert(!strcmp(leaf,"ota/result")&&!retained);results++;}return 0;
}
#define PH_MQTT_ERROR_APP_OTA_ENCODING 12
static unsigned app_stage;
static int ph_mqtt_app_error(unsigned stage,int rc){assert(stage==12&&rc<0);app_stage=stage;return rc;}
static size_t finish(ph_writer *w){size_t n=phw_finish_fixed(w);return fail_encoding?0:n;}
#define phw_finish_fixed finish
#include "../../common/ota/mqtt_ota.inc"
#undef phw_finish_fixed
static ph_buf *request(bool extra,bool numeric){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
 if(numeric)phw_int(&w,"request_id",1);else phw_text(&w,"request_id","r1");
 phw_text(&w,"url","http://host/image");if(extra)phw_bool(&w,"unsigned",true);phw_end(&w);
 ph_buf *b=phw_take(&w);assert(b);return b;
}
int main(void){
 ph_buf *valid=request(false,false);ota_command("owner",valid->buf,valid->len,true);assert(!launches);
 ph_buf *bad=request(true,false);ota_command("owner",bad->buf,bad->len,false);ph_buf_free(bad);assert(!launches);
 bad=request(false,true);ota_command("owner",bad->buf,bad->len,false);ph_buf_free(bad);assert(!launches);
 unsigned char trailing[256];assert(valid->len+1<sizeof(trailing));memcpy(trailing,valid->buf,valid->len);trailing[valid->len]=0;
 ota_command("owner",trailing,valid->len+1,false);assert(!launches);
 const char json[]="{\"request_id\":\"r1\",\"url\":\"http://host/image\"}";
 ota_command("owner",json,sizeof(json)-1,false);assert(!launches);
 busy=true;ota_command("owner",valid->buf,valid->len,false);assert(!launches);busy=false;
 fail_publish=true;assert(ota_service("owner")==-2&&!results);assert(!ota_service("owner")&&results==1);results=0;
 ota_command("owner",valid->buf,valid->len,false);assert(launches==1);
 ota_command("owner",valid->buf,valid->len,false);assert(launches==1);
 memset(valid->buf,0,valid->len);ph_buf_free(valid);assert(!strcmp(launched_url,"http://host/image"));
 assert(!ota_service("other-owner")&&!states);
 fail_encoding=true;assert(ota_service("owner")==-1&&app_stage==12&&!states&&!results);fail_encoding=false;
 fail_publish=true;assert(ota_service("owner")==-2&&!states&&!results);
 assert(!ota_service("owner")&&states==1&&!results);assert(!ota_service("owner")&&states==1);
 current_state="failed";assert(!ota_service("owner")&&states==2&&results==1);
 assert(!ota_service("owner")&&results==1);ota_forget();assert(!ota_service("owner")&&states==2);
 puts("MQTT OTA: strict MP, retained rejection, borrowed input lifetime, dedup, busy, session, encoding/send retries and final publication passed");
}
