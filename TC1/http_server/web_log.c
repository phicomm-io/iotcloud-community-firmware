#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mico.h"


#include"http_server/web_log.h"


LogRecord log_record = { 1,{ 0 } };
char log_record_str[LOG_NUM*LOG_LEN+64] = {0};
static mico_mutex_t log_lock;
static bool log_ready;
void WebLogInit(void) { log_ready=mico_rtos_init_mutex(&log_lock)==kNoErr; }

void SetLogRecord(LogRecord* lr, char* log)
{
    if(!log_ready) {free(log);return;}
    mico_rtos_lock_mutex(&log_lock);
    log[LOG_LEN-1]=0;
    char **slot=&lr->logs[(++lr->idx)%LOG_NUM];
    free(*slot); *slot=log;
    mico_rtos_unlock_mutex(&log_lock);
}

char* GetLogRecord(void)
{
    if(!log_ready) return "Logging unavailable";
    mico_rtos_lock_mutex(&log_lock);
    int first=log_record.idx-LOG_NUM+1;
    if(first<0)first=0;
    size_t used=snprintf(log_record_str,sizeof(log_record_str),"%d\n",log_record.idx);
    for(int i=first;i<=log_record.idx;++i) {
        const char *line=log_record.logs[i%LOG_NUM];
        if(!line)continue;
        size_t remaining=sizeof(log_record_str)-used;
        int n=snprintf(log_record_str+used,remaining,"%s\n",line);
        if(n<0 || (size_t)n>=remaining) {used=sizeof(log_record_str)-1;break;}
        used+=n;
    }
    snprintf(log_record_str+used,sizeof(log_record_str)-used,"FreeMem %lu bytes\n",(unsigned long)MicoGetMemoryInfo()->free_memory);
    mico_rtos_unlock_mutex(&log_lock);
    return log_record_str;
}

void WebLog(const char *M, ...)
{
    char* buff = (char*)malloc(sizeof(char)*LOG_LEN);

    if (!buff) return;

    if(!log_ready) {free(buff);return;}
    mico_rtos_lock_mutex(&log_lock);
    time_t now = time(NULL) + 28800; //东8区
    strftime(buff, TIME_LEN, "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    buff[TIME_LEN - 1] = ' ';
    mico_rtos_unlock_mutex(&log_lock);

    va_list ap;
    va_start(ap, M);
    vsnprintf(buff + TIME_LEN, LOG_LEN - TIME_LEN, M, ap);
    va_end(ap);

    SetLogRecord(&log_record, buff);
}

