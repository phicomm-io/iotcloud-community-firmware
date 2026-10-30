/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* DHCP/mDNS identity is independent of the user-editable display name. */
static inline bool ph_device_hostname(char *out,size_t capacity,const char *model,const char *id)
{
    if(!out||!capacity)return false;
    out[0]=0;
    if(!model||!id||strlen(id)!=12)return false;
    uint8_t mac[6]={0};
    for(unsigned i=0;i<12;i++){
        unsigned c=(unsigned char)id[i],v;
        if(c>='0'&&c<='9')v=c-'0';
        else if(c>='a'&&c<='f')v=c-'a'+10;
        else if(c>='A'&&c<='F')v=c-'A'+10;
        else return false;
        mac[i/2]=(uint8_t)((mac[i/2]<<4)|v);
    }
    if((mac[0]&1)||!(mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5])||!(mac[3]|mac[4]|mac[5]))return false;
    size_t n=strlen(model),length=16+n;
    if(!n||length>63||capacity<=length||model[0]=='-'||model[n-1]=='-')return false;
    for(size_t i=0;i<n;i++)if(!((model[i]>='a'&&model[i]<='z')||(model[i]>='0'&&model[i]<='9')||model[i]=='-'))return false;
    memcpy(out,"iotcloud-",9);memcpy(out+9,model,n);out[9+n]='-';
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<3;i++){out[10+n+i*2]=hex[mac[3+i]>>4];out[11+n+i*2]=hex[mac[3+i]&15];}
    out[16+n]=0;
    return true;
}
