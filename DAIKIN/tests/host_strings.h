#pragma once
/* Arduino's bounded copy for host fixtures, including pre-glibc-2.38 CI. */
#include <cstring>
static size_t test_strlcpy(char *out,const char *source,size_t capacity)
{
    size_t length=std::strlen(source);
    if(capacity){
        size_t copied=length<capacity?length:capacity-1;
        std::memcpy(out,source,copied);
        out[copied]=0;
    }
    return length;
}
#define strlcpy test_strlcpy
