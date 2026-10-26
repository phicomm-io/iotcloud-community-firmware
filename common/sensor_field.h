/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_SENSOR_FIELD_H
#define PH_SENSOR_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSOR_FIELD_MAX 16u
#define SENSOR_FIELD_ID_MAX 32u
#define SENSOR_FIELD_TYPE_MAX 16u
#define SENSOR_FIELD_NAME_MAX 64u
#define SENSOR_FIELD_UNIT_MAX 24u

static inline bool sensor_field_token(const char *s,size_t n,size_t limit)
{
    if(!s || !n || n>limit || s[0]<'a' || s[0]>'z')return false;
    for(size_t i=1;i<n;i++)if(!((s[i]>='a'&&s[i]<='z') ||
        (s[i]>='0'&&s[i]<='9') || s[i]=='_'))return false;
    return true;
}

/* JSON strings and persisted records share byte limits, valid UTF-8 and a
 * C0/DEL exclusion. Reject overlong encodings, surrogates and embedded NUL. */
static inline bool sensor_field_text(const char *s,size_t n,size_t min,size_t max)
{
    if(!s || n<min || n>max)return false;
    for(size_t i=0;i<n;){
        uint32_t cp=(unsigned char)s[i++],lowest=0;unsigned remaining=0;
        if(cp<0x80){if(cp<0x20 || cp==0x7f)return false;continue;}
        if(cp>=0xc2 && cp<=0xdf){cp&=0x1f;remaining=1;lowest=0x80;}
        else if(cp>=0xe0 && cp<=0xef){cp&=0x0f;remaining=2;lowest=0x800;}
        else if(cp>=0xf0 && cp<=0xf4){cp&=7;remaining=3;lowest=0x10000;}
        else return false;
        if(n-i<remaining)return false;
        while(remaining--){unsigned c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&0x3f);}
        if(cp<lowest || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;
    }
    return true;
}
#endif
