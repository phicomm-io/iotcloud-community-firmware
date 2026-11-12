/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included after the actual dispatcher, never a second mutation/parser path. */
#include "lan_policy.h"
ph_buf *service_lan_try_command_mp(const void *data,size_t length,const ph_auth_context *context,bool *admitted)
{
    if(admitted)*admitted=true;
    mp_request r;memset(&r,0,sizeof(r));
    if(length>1024||!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");
    if(!service_lan_context_valid(context))return service_error_mp(r.id,"auth_required");
    if(!r.id[0])return service_error_mp(r.id,"invalid_params");
    if(!ph_lan_action_allowed(r.action))return service_error_mp(r.id,"unsupported");
    /* Fixed-width fields avoid concatenation ambiguity and keep source/slot/
     * generation isolated. Wire request_id is preserved verbatim. The peer may
     * choose an id identical to HTTP/MQTT or a different peer without aliasing
     * their receipt. Raw-body digest still detects same-id changed parameters. */
    unsigned char domain[32],digest[32];
    static const char hex[]="0123456789abcdef";
    ph_lan_receipt_domain(domain,context);
    SHA256Context sha;
    if(SHA256Reset(&sha)||SHA256Input(&sha,domain,sizeof(domain))||
       SHA256Input(&sha,(const unsigned char *)r.id,strlen(r.id))||SHA256Result(&sha,digest))return service_error_mp(r.id,"busy");
    for(unsigned i=0;i<32;i++){r.receipt_key[2*i]=hex[digest[i]>>4];r.receipt_key[2*i+1]=hex[digest[i]&15];}r.receipt_key[64]=0;
    return mp_dispatch(&r,data,length,false,context,admitted);
}
ph_buf *service_lan_command_mp(const void *data,size_t length,const ph_auth_context *context)
{return service_lan_try_command_mp(data,length,context,NULL);}
