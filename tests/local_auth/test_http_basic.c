#include "../../common/httpd/http.h"
#include "../../common/httpd/basic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    const char *valid="Basic YWRtaW46cHc=";
    assert(ph_http_basic(valid,"admin","pw")&&ph_http_basic_match(valid,"admin","pw"));
    assert(ph_http_basic("Basic YWRtaW46cA==","admin","p"));
    assert(ph_http_basic("Basic YWRtaW46cHdk","admin","pwd"));
    assert(ph_http_basic("Basic YWRtaW46YTo6Yg==","admin","a::b"));
    const char *bad[]={NULL,"","basic YWRtaW46cHc=","Basic YWRtaW46cHc=extra",
        "Basic YWRtaW46cHc= ","Basic YWRtaW46cHc","Basic YWRtaW46cHc==",
        "Basic dXNlcjpwdw==","Basic YWRtaW46cHg=","Bearer YWRtaW46cHc="};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        assert(!ph_http_basic(bad[i],"admin","pw"));
        assert(!ph_http_basic_match(bad[i],"admin","pw"));
    }
    /* Retain the existing parser API's empty-password direct-access rule. */
    assert(ph_http_basic(NULL,"admin","")&&ph_http_basic("Basic wrong","admin",""));
    char too_long[73];memset(too_long,'x',72);too_long[72]=0;
    assert(!ph_http_basic(valid,"admin",too_long));
    puts("HTTP Basic: canonical encoding, credentials, padding and malformed input PASS");
    return 0;
}
