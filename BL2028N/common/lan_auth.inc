/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included by product main, once. Synchronous borrowed views only. */
#ifndef PH_LAN_AUTH_TYPES_PROVIDED
#include "local_auth.h"
#include "typed_wire.h"
#endif
#include <string.h>
static void lan_auth_hex(const uint8_t bytes[16],char text[33])
{
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++){text[2*i]=digits[bytes[i]>>4];text[2*i+1]=digits[bytes[i]&15];}
    text[32]=0;
}
static bool lan_auth_peer(const ph_value *request,char peer[13])
{
    ph_value value;
    if(!phv_get(request,"src",&value)||!phv_text(&value,peer,13)||strlen(peer)!=12)return false;
    for(unsigned i=0;i<12;i++)if(!((peer[i]>='0'&&peer[i]<='9')||(peer[i]>='a'&&peer[i]<='f')))return false;
    return true;
}
static bool lan_auth_secret(const ph_value *request,const char *key,char text[33])
{
    ph_value value;const void *bytes;size_t length;
    if(!phv_get(request,key,&value)||!phv_bytes(&value,&bytes,&length)||length!=16)return false;
    lan_auth_hex((const uint8_t *)bytes,text);return true;
}
bool feature_lan_context(const char *peer,const uint8_t token[16],ph_auth_context *ctx)
{
    ph_auth_state *s=device_local_auth();char text[33];
    if(!s||!token||!ctx)return false;
    lan_auth_hex(token,text);
    bool ok=ph_auth_udp_check(s,peer,text,ctx);
    memset(text,0,sizeof(text));return ok;
}
bool feature_lan_auth(const char *peer,const uint8_t token[16],unsigned scopes)
{
    ph_auth_context ctx;
    return feature_lan_context(peer,token,&ctx)&&ph_auth_context_valid(&ctx,scopes);
}
bool feature_lan_pair(const ph_value *request,ph_writer *reply,const char *ip)
{
    ph_value value;char op[16],peer[13],device[13],secret[33]={0},output[33]={0};
    /* Product/cloud identifiers can retain their historical upper-case MAC.
     * LAN discovery, strict sender jobs and all wire identities are lower-case. */
    if(strlen(AUTH_DEVICE_ID)!=12)return false;
    for(unsigned i=0;i<12;i++){char c=AUTH_DEVICE_ID[i];device[i]=c>='A'&&c<='F'?c+('a'-'A'):c;}device[12]=0;
    const char *code="invalid_params",*result_key=NULL;bool ok=false;
    uint8_t nonce[16];const void *nonce_view;size_t nonce_length;
    if(!phv_get(request,"op",&value)||!phv_text(&value,op,sizeof(op))||
       (strcmp(op,"auth_pair")&&strcmp(op,"auth_resume")&&strcmp(op,"auth_unpair")))return false;
    if(!phv_get(request,"nonce",&value)||!phv_bytes(&value,&nonce_view,&nonce_length)||nonce_length!=16)return true;
    memcpy(nonce,nonce_view,16);
    ph_auth_state *s=device_local_auth();
    if(!s){code="busy";goto finish;}
    if(!lan_auth_peer(request,peer))goto finish;
    if(!strcmp(op,"auth_pair")){
        int64_t scopes;
        if(!phv_keys(request,"|op||src||password||scopes||nonce|")||
           !phv_get(request,"password",&value)||!phv_text(&value,secret,sizeof(secret))||
           !phv_get(request,"scopes",&value)||!phv_int(&value,&scopes)||
           scopes<1||scopes>(PH_AUTH_SCOPE_CONTROL|PH_AUTH_SCOPE_SENSOR|PH_AUTH_SCOPE_GROUP))goto finish;
        ok=ph_auth_pair(s,secret,peer,(unsigned)scopes,output);result_key="key";
        code=ok?"ok":"auth_required";
    }else if(!strcmp(op,"auth_resume")){
        ph_value boot_value;uint32_t boot=0;int64_t number;
        if(!phv_keys(request,"|op||src||key||nonce||boot|")||!lan_auth_secret(request,"key",secret))goto finish;
        bool business=phv_get(request,"boot",&boot_value);
        if(business){if(!phv_int(&boot_value,&number)||number<1||number>UINT32_MAX)goto finish;boot=(uint32_t)number;}
        ok=business?ph_auth_pair_resume_boot(s,peer,secret,boot,output):ph_auth_pair_resume(s,peer,secret,output);result_key="token";code=ok?"ok":"auth_required";
    }else{
        if(!phv_keys(request,"|op||src||token||nonce|")||!lan_auth_secret(request,"token",secret))goto finish;
        ok=ph_auth_pair_unpair_request(s,peer,secret,ip,nonce);code=ok?"ok":"auth_required";
    }
finish:
    phw_begin(reply,NULL,false);phw_text(reply,"op",op);phw_text(reply,"code",code);
    phw_text(reply,"src",device);phw_bytes_value(reply,"nonce",nonce,16);
    if(ok&&result_key){uint8_t bytes[16];if(ph_auth_token_decode(output,bytes))phw_bytes_value(reply,result_key,bytes,16);memset(bytes,0,sizeof(bytes));}
    phw_end(reply);memset(secret,0,sizeof(secret));memset(output,0,sizeof(output));return true;
}
