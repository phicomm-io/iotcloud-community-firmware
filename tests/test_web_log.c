#include "mico.h"
#include <time.h>
static struct { unsigned free_memory; } memory_info={12345};
#define MicoGetMemoryInfo() (&memory_info)
#define mico_rtos_suspend_all_thread() ((void)0)
#define mico_rtos_resume_all_thread() ((void)0)
#include "../TC1/http_server/web_log.c"
int main(void){
 assert(!strcmp(GetLogRecord(),"Logging unavailable"));
 WebLog("before init");assert(log_record.idx==0);
 WebLogInit();assert(log_record.idx==1 && log_ready);
 for(unsigned i=0;i<LOG_NUM;i++)assert(!log_record.logs[i][0]);
 WebLog("first %d",42);assert(log_record.idx==2);
 assert(strstr(GetLogRecord(),"first 42\n") && strstr(GetLogRecord(),"FreeMem 12345 bytes\n"));
 for(unsigned i=0;i<80;i++)WebLog("entry %03u",i);
 const char *text=GetLogRecord();
 assert(!strncmp(text,"82\n",3));assert(!strstr(text,"entry 015\n"));
 assert(strstr(text,"entry 016\n") && strstr(text,"entry 079\n"));
 assert(strstr(text,"entry 016\n")<strstr(text,"entry 079\n") && !log_lock);
 puts("TC1 log: startup index, initially empty ring, formatting and 64-line wrap passed");
 return 0;
}
