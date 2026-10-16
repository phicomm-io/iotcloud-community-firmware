#include "mico.h"
#include "../../common/ota/fetch.h"
static uint32_t started,cancel_after;
static FILE *output;
void ota_job_sample(void){}
bool ota_job_cancelled(void){return cancel_after && (uint32_t)(mico_rtos_get_time()-started)>=cancel_after;}
static bool begin(void *ctx,uint32_t n){(void)ctx;printf("length=%u ",n);return true;}
static bool write_data(void *ctx,uint32_t at,const uint8_t *p,size_t n){(void)ctx;(void)at;return fwrite(p,1,n,output)==n;}
int main(int argc,char **argv){if(argc!=5)return 2;const char *error;cancel_after=atoi(argv[3]);started=mico_rtos_get_time();output=fopen(argv[4],"wb");if(!output)return 2;
 bool ok=ota_fetch(argv[1],strtoul(argv[2],NULL,10),begin,write_data,NULL,&error);fclose(output);printf("ok=%d elapsed=%u error=%s\n",ok,(uint32_t)(mico_rtos_get_time()-started),error?error:"");return ok?0:1;}
