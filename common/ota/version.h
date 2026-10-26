/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PHICOMM_OTA_VERSION_H
#define PHICOMM_OTA_VERSION_H
#include <stdbool.h>
#include <limits.h>
#include <string.h>

/* Published versions are core[-rc[.N]]. Existing build identities append a
 * hexadecimal commit and optional -dirty; +metadata never changes precedence. */
typedef struct {
    unsigned core[3], candidate;
    bool rc, numbered;
} ph_ota_version_t;

static inline bool ph_ota_version_number(const char **input, unsigned *out)
{
    const char *s=*input;
    unsigned value=0;
    if(*s<'0' || *s>'9' || (*s=='0' && s[1]>='0' && s[1]<='9'))return false;
    do {
        unsigned digit=(unsigned)(*s++-'0');
        if(value>(UINT_MAX-digit)/10u)return false;
        value=value*10u+digit;
    }while(*s>='0' && *s<='9');
    *input=s;*out=value;return true;
}

static inline bool ph_ota_version_parse(const char *s, ph_ota_version_t *out)
{
    ph_ota_version_t version={{0},0,false,false};
    if(!s || !out)return false;
    for(unsigned i=0;i<3;i++) {
        if(!ph_ota_version_number(&s,&version.core[i]))return false;
        if(i<2) {if(*s!='.')return false;s++;}
    }
    if(s[0]=='-' && s[1]=='r' && s[2]=='c') {
        version.rc=true;s+=3;
        if(*s=='.') {
            s++;version.numbered=true;
            if(!ph_ota_version_number(&s,&version.candidate))return false;
        }
    }
    if(*s=='-') {
        unsigned digits=0;s++;
        while((*s>='0' && *s<='9') || (*s>='a' && *s<='f') || (*s>='A' && *s<='F')) {s++;digits++;}
        if(digits<6 || digits>40)return false;
        if(!strncmp(s,"-dirty",6))s+=6;
    }
    if(*s=='+') {
        s++;
        for(;;) {
            const char *start=s;
            while((*s>='0' && *s<='9') || (*s>='a' && *s<='z') ||
                  (*s>='A' && *s<='Z') || *s=='-')s++;
            if(s==start)return false;
            if(*s!='.')break;
            s++;
        }
    }
    if(*s)return false;
    *out=version;return true;
}

static inline int ph_ota_version_compare(const ph_ota_version_t *a, const ph_ota_version_t *b)
{
    for(unsigned i=0;i<3;i++)if(a->core[i]!=b->core[i])return a->core[i]>b->core[i]?1:-1;
    if(a->rc!=b->rc)return a->rc?-1:1;
    if(!a->rc)return 0;
    if(a->numbered!=b->numbered)return a->numbered?1:-1;
    return a->candidate>b->candidate?1:a->candidate<b->candidate?-1:0;
}
#endif
