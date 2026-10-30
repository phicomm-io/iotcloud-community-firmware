#include "platform_host.h"
#include "port_internal.h"
#include "port.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
static void *timer_loop(void *p){(void)p;for(;;){PH_BL_Tick();usleep(20000);}return NULL;}
int main(int argc,char **argv){
 assert(argc==4);signal(SIGPIPE,SIG_IGN);mqtt_port=atoi(argv[1]);ph_host_http_port=atoi(argv[2]);
 snprintf(store_dir,sizeof(store_dir),"%s",argv[3]);mkdir(store_dir,0700);
 if(getenv("PH_HOST_EMPTY_PASSWORD"))CFG_SetWebPassword("");
 PH_BL_Init(0);assert(bl_ready);PH_BL_StartHTTP();pthread_t timer;assert(!pthread_create(&timer,NULL,timer_loop,NULL));
 puts("READY");fflush(stdout);for(;;)pause();
}
