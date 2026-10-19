#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define SERVICE_BUSY_BYTES 160
/* Only recover a validated top-level identifier for an overload reply. Never
 * execute or validate commands here. Bounded scanning uses no heap or recursion. */
static bool busy_string(const unsigned char **at, const unsigned char *end, char *out, size_t cap) {
    const unsigned char *p=*at; size_t n=0; bool fits=true;
    if (p==end || *p++!='"') return false;
    while (p<end && *p!='"') {
        unsigned c=*p++;
        if (c<32) return false;
        if (c=='\\') {
            if (p==end) return false;
            c=*p++;
            if (c=='u') {
                c=0;
                for (unsigned i=0;i<4;i++) {
                    if (p==end) return false;
                    unsigned h=*p++,v;
                    if(h>='0'&&h<='9')v=h-'0';
                    else if(h>='a'&&h<='f')v=h-'a'+10;
                    else if(h>='A'&&h<='F')v=h-'A'+10;
                    else return false;
                    c=c*16+v;
                }
                if (!c || c>127) fits=false;
            } else if(c!='"'&&c!='\\'&&c!='/') {
                if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else return false;
            }
        }
        if(out && n+1<cap)out[n]=(char)c;else if(out)fits=false;
        n++;
    }
    if(p==end)return false;
    *at=p+1;
    if(out)out[n<cap?n:cap-1]=0;
    return !out || fits;
}
static void busy_space(const unsigned char **p,const unsigned char *end) {
    while(*p<end && (**p==' '||**p=='\t'||**p=='\r'||**p=='\n'))(*p)++;
}
static bool busy_identifier(const char *p) {
    if(!*p)return false;
    for(;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='.'||*p=='-'))return false;
    return true;
}
static void service_busy_id(const void *payload,size_t length,char id[65]) {
    const unsigned char *p=payload,*end;bool valid=false,complete=false;id[0]=0;
    if(!payload)goto done;end=p+length;
    if(length>8192)goto done;
    busy_space(&p,end);if(p==end||*p++!='{')goto done;
    for(;;) {
        char key[16];busy_space(&p,end);if(p==end)goto done;
        if(*p=='}'){p++;break;}
        bool key_ok=busy_string(&p,end,key,sizeof(key));
        /* Oversized keys are irrelevant; busy_string still advances to their end. */
        if(p==end)goto done;
        busy_space(&p,end);if(p==end||*p++!=':')goto done;busy_space(&p,end);
        const unsigned char *value=p;
        if(key_ok && !strcmp(key,"request_id")) {
            valid=busy_string(&p,end,id,65) && busy_identifier(id);
            p=value;
        }
        unsigned depth=0;bool consumed=false;
        while(p<end) {
            if(*p=='"'){if(!busy_string(&p,end,NULL,0))goto done;consumed=true;continue;}
            if(*p=='{'||*p=='['){if(++depth>32)goto done;}
            else if(*p=='}'||*p==']'){if(!depth)break;depth--;}
            else if(*p==','&&!depth)break;
            p++;consumed=true;
        }
        if(!consumed||depth||p==end)goto done;
        if(*p=='}'){p++;break;}
        if(*p++!=',')goto done;
    }
    busy_space(&p,end);complete=p==end;
 done:
    if(!valid || !complete)id[0]=0;
}
static void service_busy_format(const char *id,char out[SERVICE_BUSY_BYTES]) {
    if(*id) {strcpy(out,"{\"request_id\":\"");strcat(out,id);strcat(out,"\",\"code\":\"busy\",\"data\":{}}");}
    else strcpy(out,"{\"code\":\"busy\",\"data\":{}}");
}
static inline void service_busy_wire(const void *payload,size_t length,char out[SERVICE_BUSY_BYTES]) {
    char id[65];service_busy_id(payload,length,id);service_busy_format(id,out);
}
