/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "service_mp.h"
#include <limits.h>

/* HA command templates emit only maps, strings, booleans and integer settings.
 * This restricted JSON bridge is isolated from HTTP/cloud MQTT. It borrows
 * bytes synchronously, uses bounded scratch and never builds a JSON graph. */
typedef struct {const unsigned char *p,*end;unsigned nodes;} ha_reader;
static inline void hac_ws(ha_reader *r)
{while(r->p<r->end && (*r->p==' ' || *r->p=='\t' || *r->p=='\n' || *r->p=='\r'))r->p++;}
static inline int hac_hex(unsigned char c)
{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static inline bool hac_u16(ha_reader *r,unsigned *out)
{if(r->end-r->p<4)return false;*out=0;for(unsigned i=0;i<4;i++){int n=hac_hex(*r->p++);if(n<0)return false;*out=(*out<<4)|(unsigned)n;}return true;}
static inline bool hac_string(ha_reader *r,char *out,size_t capacity)
{
    if(r->p==r->end || *r->p++!='"')return false;
    size_t n=0;
    while(r->p<r->end){
        unsigned c=*r->p++;
        if(c=='"'){out[n]=0;return true;}
        if(c<32)return false;
        if(c=='\\'){
            if(r->p==r->end)return false;c=*r->p++;
            if(c=='u'){
                unsigned cp;if(!hac_u16(r,&cp))return false;
                if(cp>=0xd800 && cp<=0xdbff){
                    if(r->end-r->p<6 || r->p[0]!='\\' || r->p[1]!='u')return false;
                    r->p+=2;unsigned low;if(!hac_u16(r,&low)||low<0xdc00||low>0xdfff)return false;
                    cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
                }else if(cp>=0xdc00 && cp<=0xdfff)return false;
                if(!cp)return false; /* no embedded C-string NUL */
                unsigned count=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
                if(count>=capacity-n)return false;
                if(count==1)out[n++]=(char)cp;
                else{
                    out[n++]=(char)((count==2?0xc0:count==3?0xe0:0xf0)|(cp>>(6*(count-1))));
                    for(unsigned i=count-1;i;i--)out[n++]=(char)(0x80|((cp>>(6*(i-1)))&63));
                }
                continue;
            }
            switch(c){case '"':case '\\':case '/':break;case 'b':c='\b';break;case 'f':c='\f';break;case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;default:return false;}
        }
        if(n+1>=capacity)return false;out[n++]=(char)c;
    }
    return false;
}
static inline bool hac_value(ha_reader *r,ph_writer *w,const char *key,unsigned depth)
{
    hac_ws(r);if(depth>6 || ++r->nodes>64 || r->p==r->end)return false;
    if(*r->p=='{'){
        r->p++;phw_begin(w,key,false);hac_ws(r);
        if(r->p<r->end && *r->p=='}'){r->p++;phw_end(w);return !w->failed;}
        do{
            char name[48];if(!hac_string(r,name,sizeof(name)))return false;
            hac_ws(r);if(r->p==r->end || *r->p++!=':')return false;
            if(!hac_value(r,w,name,depth+1))return false;hac_ws(r);
            if(r->p==r->end)return false;
            unsigned c=*r->p++;if(c=='}'){phw_end(w);return !w->failed;}if(c!=',')return false;hac_ws(r);
        }while(r->p<r->end);
        return false;
    }
    if(*r->p=='"'){char value[128];if(!hac_string(r,value,sizeof(value)))return false;phw_text(w,key,value);return !w->failed;}
    if(r->end-r->p>=4 && !memcmp(r->p,"true",4)){r->p+=4;phw_bool(w,key,true);return !w->failed;}
    if(r->end-r->p>=5 && !memcmp(r->p,"false",5)){r->p+=5;phw_bool(w,key,false);return !w->failed;}
    bool negative=*r->p=='-';if(negative && ++r->p==r->end)return false;
    if(*r->p<'0'||*r->p>'9')return false;
    bool zero=*r->p=='0';uint64_t value=0;unsigned digits=0;
    do{if((zero && digits)||digits==16)return false;value=value*10+(*r->p++-'0');digits++;}while(r->p<r->end && *r->p>='0' && *r->p<='9');
    if(value>9007199254740991ULL)return false;
    phw_int(w,key,negative?-(int64_t)value:(int64_t)value);return !w->failed;
}
static inline size_t ha_command_pack(const void *data,size_t length,void *output,size_t capacity)
{
    if(!data || !length || length>1024 || memchr(data,0,length))return 0;
    ha_reader r={(const unsigned char *)data,(const unsigned char *)data+length,0};
    ph_writer w;ph_buf b;if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,output,capacity))return 0;
    hac_ws(&r);if(r.p==r.end||*r.p!='{'||!hac_value(&r,&w,NULL,0))return 0;
    hac_ws(&r);if(r.p!=r.end)return 0;
    size_t n=phw_finish_fixed(&w);ph_value checked;
    /* The MP core enforces duplicate keys, UTF8 and numeric/profile limits. */
    if(!n || !phv_parse(PH_WIRE_MSGPACK,output,n,&checked))return 0;
    return n;
}
static inline void ha_command_input(const void *data,size_t length)
{
    /* Only the product MQTT owner calls this synchronous adapter. Keeping
     * its scratch in owner storage avoids retaining 1536 stack bytes during
     * business dispatch; borrowed values never escape service_command_mp. */
    static unsigned char bytes[1536];
    size_t n=ha_command_pack(data,length,bytes,sizeof(bytes));
    if(n){ph_buf *response=service_command_mp(bytes,n);ph_buf_free(response);}
}
