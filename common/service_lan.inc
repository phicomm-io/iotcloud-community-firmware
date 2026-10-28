/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included after the actual dispatcher, never a second mutation/parser path. */
static bool service_lan_action(const char *action)
{
    /* Explicit operations only: never infer privilege from a .get suffix or
     * pass a domain that could later acquire configuration/OTA semantics. */
    static const char *const allowed[]={
        "status.get","capabilities.get","info.get","diagnostics.get",
        "power.get","filter.get","alarm.get","wake_alarm.get","mcu.version.get",
        "fan.set","fan.mode.set","fan.mode.toggle","light.set","child_lock.set",
        "light_always.set","light_effect.select","light_effect.next",
        "outlet.set","indicator.set","display.set","buzzer.set",
        "wake_alarm.start","wake_alarm.stop",
        "fresh_air.config.get","fresh_air.linkage.get","fresh_air.set",NULL
    };
    for(unsigned i=0;allowed[i];i++)if(!strcmp(action,allowed[i]))return true;
    return false;
}
ph_buf *service_lan_try_command_mp(const void *data,size_t length,const ph_auth_context *context,bool *admitted)
{
    if(admitted)*admitted=true;
    mp_request r;memset(&r,0,sizeof(r));
    if(length>1024||!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");
    if(!service_lan_context_valid(context))return service_error_mp(r.id,"auth_required");
    if(!r.id[0])return service_error_mp(r.id,"invalid_params");
    if(!service_lan_action(r.action))return service_error_mp(r.id,"unsupported");
    /* Fixed-width fields avoid concatenation ambiguity and keep source/slot/
     * generation isolated. Wire request_id is preserved verbatim. The peer may
     * choose an id identical to HTTP/MQTT or a different peer without aliasing
     * their receipt. Raw-body digest still detects same-id changed parameters. */
    unsigned char domain[32]={0},digest[32];
    static const char hex[]="0123456789abcdef";
    memcpy(domain,"lan-v4",6);memcpy(domain+8,context->peer,13);
    domain[21]=(unsigned char)context->source;domain[22]=context->slot;
    for(unsigned i=0;i<4;i++)domain[24+i]=(unsigned char)(context->generation>>(24-8*i));
    SHA256Context sha;
    if(SHA256Reset(&sha)||SHA256Input(&sha,domain,sizeof(domain))||
       SHA256Input(&sha,(const unsigned char *)r.id,strlen(r.id))||SHA256Result(&sha,digest))return service_error_mp(r.id,"busy");
    for(unsigned i=0;i<32;i++){r.receipt_key[2*i]=hex[digest[i]>>4];r.receipt_key[2*i+1]=hex[digest[i]&15];}r.receipt_key[64]=0;
    return mp_dispatch(&r,data,length,false,context,admitted);
}
ph_buf *service_lan_command_mp(const void *data,size_t length,const ph_auth_context *context)
{return service_lan_try_command_mp(data,length,context,NULL);}
