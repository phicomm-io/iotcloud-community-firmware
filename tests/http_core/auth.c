#include "auth.h"
#include <assert.h>
#include <string.h>
static int depth;
void mico_rtos_suspend_all_thread(void){assert(depth++==0);}
void mico_rtos_resume_all_thread(void){assert(--depth==0);}
int main(void){
 assert(!tc1_http_authorized("")); /* configured first boot cannot fail open */
 assert(httpd_auth_init(NULL,"pw")<0);assert(!tc1_http_authorized(""));
 assert(httpd_auth_init("admin","pw")==0);
 assert(tc1_http_authorized("Basic YWRtaW46cHc="));assert(!tc1_http_authorized(""));
 char long_password[100];memset(long_password,'a',99);long_password[99]=0;
 assert(httpd_auth_init("admin",long_password)<0);
 assert(tc1_http_authorized("Basic YWRtaW46cHc=")); /* failed replace retains old */
 assert(httpd_auth_init("","")==0);assert(tc1_http_authorized(""));assert(!depth);
 return 0;
}
