/* Included by each MQTT service. All state and publication stay on its MQTT thread.
 * Requires ota_launch(url), ota_snapshot(state,error,progress), ota_publish(leaf,json,retain).
 */
#include "../json_build.h"

static char ota_request[33], ota_seen[8][33], ota_owner[192], ota_last[384];
static unsigned ota_seen_next;
static bool ota_result_pending;
static char ota_rejected[33],ota_rejected_owner[192];
static void ota_command(const char *owner,const void *payload,size_t length,bool retained)
{
    if(retained || !length || length>640 || memchr(payload,0,length)) return;
    char text[641]; memcpy(text,payload,length);text[length]=0;
    json_tokener *tok=json_tokener_new(); if(!tok)return;
    json_object *o=json_tokener_parse_ex(tok,text,length);
    bool valid=tok->err==json_tokener_success && o && json_object_is_type(o,json_type_object);
    unsigned end=tok->char_offset; while(end<length && isspace((unsigned char)text[end]))end++;
    valid=valid && end==length; json_tokener_free(tok);
    if(!valid) {if(o)json_object_put(o);return;}
    json_object *id=json_object_object_get(o,"request_id"),*url=json_object_object_get(o,"url");
    valid=id && url && json_object_is_type(id,json_type_string) && json_object_is_type(url,json_type_string);
    json_object_object_foreach(o,k,v) { (void)v; if(strcmp(k,"request_id") && strcmp(k,"url"))valid=false; }
    const char *rid=valid?json_object_get_string(id):"", *link=valid?json_object_get_string(url):"";
    size_t n=strlen(rid);
    if(valid && ((size_t)json_object_get_string_len(id)!=n ||
        (size_t)json_object_get_string_len(url)!=strlen(link)))valid=false;
    if(!n || n>32 || !strlen(link) || strlen(link)>499)valid=false;
    for(unsigned i=0;i<n;i++)if(!isalnum((unsigned char)rid[i]) && rid[i]!='-' && rid[i]!='_')valid=false;
    for(unsigned i=0;i<8;i++)if(!strcmp(ota_seen[i],rid))valid=false;
    if(valid && ota_launch(link)) {
        snprintf(ota_request,sizeof(ota_request),"%s",rid);
        snprintf(ota_seen[ota_seen_next++%8],33,"%s",rid);
        snprintf(ota_owner,sizeof(ota_owner),"%s",owner);
        ota_last[0]=0; ota_result_pending=true;
    } else if(valid) {
        snprintf(ota_rejected,sizeof(ota_rejected),"%s",rid);
        snprintf(ota_rejected_owner,sizeof(ota_rejected_owner),"%s",owner);
    }
    json_object_put(o);
}
static int ota_service(const char *owner)
{
    if(ota_rejected[0] && !strcmp(owner,ota_rejected_owner)) {
        char rejected[160];snprintf(rejected,sizeof(rejected),
            "{\"request_id\":\"%s\",\"state\":\"rejected\",\"error\":\"busy or unsupported URL\"}",ota_rejected);
        int result=ota_publish("ota/result",rejected,false);
        if(result)return result;ota_rejected[0]=0;
    }
    if(!ota_request[0] || strcmp(owner,ota_owner))return 0;
    char state[32],error[128],text[384];unsigned progress;
    ota_snapshot(state,error,&progress);
    json_object *o=json_object_new_object();if(!o)return -1;
    json_build_object_add(o,"request_id",json_object_new_string(ota_request));
    json_build_object_add(o,"state",json_object_new_string(state));
    json_build_object_add(o,"error",json_object_new_string(error));
    json_build_object_add(o,"progress",json_object_new_int(progress));
    const char *wire=json_object_to_json_string(o);if(!wire){json_object_put(o);return -1;}
    snprintf(text,sizeof(text),"%s",wire);json_object_put(o);
    int result=0;
    if(strcmp(text,ota_last)) {
        result=ota_publish("ota/state",text,true);
        if(!result)snprintf(ota_last,sizeof(ota_last),"%s",text);
    }
    if(!result && ota_result_pending && (!strcmp(state,"ready") || !strcmp(state,"failed"))) {
        result=ota_publish("ota/result",text,false);if(!result)ota_result_pending=false;
    }
    return result;
}

static void ota_forget(void) { ota_request[0]=ota_rejected[0]=0;ota_result_pending=false; }
