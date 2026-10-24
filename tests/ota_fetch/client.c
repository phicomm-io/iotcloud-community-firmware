#include "mico.h"
#include <signal.h>
#include <dirent.h>
#include <assert.h>
#include "../../common/ota/fetch.h"
extern bool ota_test_clean(void);
#undef shutdown
int ota_test_shutdown(int fd,int how){
 (void)fd;(void)how;
 assert(!"application shutdown may corrupt a concurrent kernel TLS recv");
 return -1;
}
#undef setsockopt
int ota_test_setsockopt(int fd,int level,int option,const void *value,socklen_t length){
 if(level==SOL_SOCKET && (option==SO_RCVTIMEO || option==SO_SNDTIMEO)){
  assert(length==sizeof(int));int ms=*(const int *)value;struct timeval tv={ms/1000,(ms%1000)*1000};
  return setsockopt(fd,level,option,&tv,sizeof(tv));
 }
 return setsockopt(fd,level,option,value,length);
}
#undef connect
int ota_test_connect(int fd,const struct sockaddr *address,socklen_t length){
 int rc=connect(fd,address,length);
 /* Model the proprietary kernel: application errno is unrelated to socket
  * failure, even while an ordinary nonblocking connection is in progress. */
 if(getenv("OTA_TEST_FAULT")&&!strcmp(getenv("OTA_TEST_FAULT"),"socket_errno"))errno=0;
 return rc;
}
static uint32_t started,cancel_after;
static FILE *output;
void ota_job_sample(void){}
bool ota_job_cancelled(void){return cancel_after && (uint32_t)(mico_rtos_get_time()-started)>=cancel_after;}
static bool begin(void *ctx,uint32_t n){(void)ctx;printf("length=%u ",n);return true;}
static bool write_data(void *ctx,uint32_t at,const uint8_t *p,size_t n){(void)ctx;(void)at;return fwrite(p,1,n,output)==n;}
static unsigned open_fds(void){DIR *directory=opendir("/proc/self/fd");assert(directory);unsigned n=0;while(readdir(directory))n++;closedir(directory);return n;}
int main(int argc,char **argv){if(argc!=5&&argc!=6)return 2;signal(SIGPIPE,SIG_IGN);const char *error;
 unsigned before=open_fds(),attempts=argc==6?atoi(argv[5]):1;bool ok=false;
 for(unsigned i=0;i<attempts;i++) {
  cancel_after=atoi(argv[3]);started=mico_rtos_get_time();output=fopen(argv[4],"wb");if(!output)return 2;
  ok=ota_fetch(argv[1],strtoul(argv[2],NULL,10),begin,write_data,NULL,&error);fclose(output);
  printf("ok=%d elapsed=%u error=%s\n",ok,(uint32_t)(mico_rtos_get_time()-started),error?error:"");
  assert(ota_test_clean());assert(open_fds()==before);
 }
 return ok?0:1;}
