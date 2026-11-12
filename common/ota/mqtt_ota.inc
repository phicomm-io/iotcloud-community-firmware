/* MQTT OTA control uses MessagePack; only immutable owned snapshots survive calls.
 * Requires ota_launch(url), ota_snapshot(state,error,progress),
 * ota_publish(leaf,bytes,length,retain), and mqtt/client.h application errors.
 */
#include "../typed_wire.h"

static char ota_request[33],ota_seen[8][33],ota_owner[192];
static unsigned char ota_last[384];
static size_t ota_last_length;
static unsigned ota_seen_next;
static bool ota_result_pending;
static char ota_rejected[33],ota_rejected_owner[192];
static void ota_command(const char *owner,const void *payload,size_t length,bool retained)
{
    ph_value root,id,url;char rid[33],link[500];
    if(retained || !owner || !length || length>640 ||
       !phv_parse(PH_WIRE_MSGPACK,payload,length,&root) ||
       !phv_keys(&root,"|request_id||url|") ||
       !phv_get(&root,"request_id",&id) || !phv_text(&id,rid,sizeof(rid)) ||
       !phv_get(&root,"url",&url) || !phv_text(&url,link,sizeof(link)) ||
       !rid[0] || !link[0] || strlen(owner)>=sizeof(ota_owner))return;
    for(unsigned i=0;rid[i];i++)if(!((rid[i]>='a'&&rid[i]<='z') ||
       (rid[i]>='A'&&rid[i]<='Z') || (rid[i]>='0'&&rid[i]<='9') || rid[i]=='-' || rid[i]=='_'))return;
    for(unsigned i=0;i<8;i++)if(!strcmp(ota_seen[i],rid))return;
    if(ota_launch(link)){
        strcpy(ota_request,rid);strcpy(ota_seen[ota_seen_next++%8],rid);
        strcpy(ota_owner,owner);ota_last_length=0;ota_result_pending=true;
    }else{
        strcpy(ota_rejected,rid);strcpy(ota_rejected_owner,owner);
    }
}
static int ota_service(const char *owner)
{
    if(ota_rejected[0] && !strcmp(owner,ota_rejected_owner)){
        unsigned char bytes[160];ph_writer w;ph_buf storage;phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,bytes,sizeof(bytes));
        phw_begin(&w,NULL,false);phw_text(&w,"request_id",ota_rejected);phw_text(&w,"state","rejected");
        phw_text(&w,"error","busy or unsupported URL");phw_end(&w);
        size_t length=phw_finish_fixed(&w);if(!length)return ph_mqtt_app_error(PH_MQTT_ERROR_APP_OTA_ENCODING,-1);
        int result=ota_publish("ota/result",bytes,length,false);
        if(result)return result;ota_rejected[0]=0;
    }
    if(!ota_request[0] || strcmp(owner,ota_owner))return 0;
    char state[32],error[128];unsigned progress;unsigned char bytes[384];ph_writer w;ph_buf storage;
    ota_snapshot(state,error,&progress);
    phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,bytes,sizeof(bytes));phw_begin(&w,NULL,false);
    phw_text(&w,"request_id",ota_request);phw_text(&w,"state",state);
    phw_text(&w,"error",error);phw_u32(&w,"progress",progress);phw_end(&w);
    size_t length=phw_finish_fixed(&w);if(!length)return ph_mqtt_app_error(PH_MQTT_ERROR_APP_OTA_ENCODING,-1);
    int result=0;
    if(length!=ota_last_length || memcmp(bytes,ota_last,length)){
        result=ota_publish("ota/state",bytes,length,true);
        if(!result){memcpy(ota_last,bytes,length);ota_last_length=length;}
    }
    if(!result && ota_result_pending && (!strcmp(state,"ready") || !strcmp(state,"failed"))){
        result=ota_publish("ota/result",bytes,length,false);if(!result)ota_result_pending=false;
    }
    return result;
}
static void ota_forget(void){ota_request[0]=ota_rejected[0]=0;ota_result_pending=false;ota_last_length=0;}
