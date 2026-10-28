/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bonjour_lan.h"
#include "lan_wire_v4.h"
#include <string.h>
#include <stdio.h>
/* MiCO's mDNS API encodes TXT as dot-delimited labels, escaping dot/slash.
 * Values here are firmware identity constants, never SSID/password/user data. */
static bool bonjour_lan_field(char *out,size_t capacity,size_t *used,const char *key,const char *value)
{
    size_t a=strlen(key),b=value?strlen(value):0;if(!b||a+b+1>255)return false;
    size_t required=a+2;
    for(size_t i=0;i<b;i++){unsigned char c=(unsigned char)value[i];if(c<33||c>126)return false;required+=c=='.'||c=='/'?2:1;}
    if(*used>=capacity||required>=capacity-*used)return false;
    memcpy(out+*used,key,a);*used+=a;out[(*used)++]='=';
    for(size_t i=0;i<b;i++){if(value[i]=='.'||value[i]=='/')out[(*used)++]='/';out[(*used)++]=value[i];}
    out[(*used)++]='.';out[*used]=0;return true;
}
bool bonjour_lan_txt(char *out,size_t capacity,const char *id,const char *model,const char *variant,const char *firmware,uint16_t port)
{
    if(!out||!capacity||!id||strlen(id)!=12||!model||!variant||!firmware||port!=LAN_V4_PORT)return false;
    for(unsigned i=0;i<12;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
    if(strcmp(model,"a1")&&strcmp(model,"m1")&&strcmp(model,"tc1"))return false;
    size_t used=0;out[0]=0;
    return bonjour_lan_field(out,capacity,&used,"lan_wire","4")&&
        bonjour_lan_field(out,capacity,&used,"version","4")&&
        bonjour_lan_field(out,capacity,&used,"codec","msgpack")&&
        bonjour_lan_field(out,capacity,&used,"id",id)&&
        bonjour_lan_field(out,capacity,&used,"model",model)&&
        bonjour_lan_field(out,capacity,&used,"variant",variant)&&
        bonjour_lan_field(out,capacity,&used,"firmware",firmware)&&
        bonjour_lan_field(out,capacity,&used,"udpport","11288");
}
#ifndef PH_BONJOUR_LAN_CODEC_ONLY
#include "mico_system.h"
static bool bonjour_lan_registered;
static bool bonjour_lan_start(const char *id,const char *model,const char *variant,const char *firmware)
{
    if(bonjour_lan_registered){mdns_resume_record(LAN_V4_MDNS_NAME,Station);return true;}
    char txt[384],host[40],instance[40];
    if(!bonjour_lan_txt(txt,sizeof(txt),id,model,variant,firmware,LAN_V4_PORT))return false;
    int a=snprintf(host,sizeof(host),"openiot-%s.local.",id);
    int b=snprintf(instance,sizeof(instance),"openiot-%s-%s",model,id);
    if(a<0||(size_t)a>=sizeof(host)||b<0||(size_t)b>=sizeof(instance))return false;
    mdns_init_t init;memset(&init,0,sizeof(init));init.service_name=LAN_V4_MDNS_NAME;
    init.host_name=host;init.instance_name=instance;init.txt_record=txt;init.service_port=LAN_V4_PORT;
    bonjour_lan_registered=mdns_add_record(init,Station,120)==kNoErr;
    return bonjour_lan_registered;
}
static void bonjour_lan_stop(void)
{if(bonjour_lan_registered)mdns_suspend_record(LAN_V4_MDNS_NAME,Station,false);}
#endif
