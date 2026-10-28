#pragma once
#include <errno.h>
/* IDs are volatile RAM only; append vocabulary without a configuration migration. */
static const char *const log_levels[]={"info","warn","error"};
static const char *const log_modules[]={"system","command","config","button","schedule","mqtt","ota","test"};
static const char *const log_events[]={"boot","accepted","rejected","unsupported_layout","child_locked","gesture","storage_error","missed","executed","busy","power_on","software","lockup","watchdog","unknown","transfer","transition"};
static const char *const log_stages[]={"initial","ota","state","link","result","subscribe","receive"};
static int log_id(const char *s,const char *const *table,unsigned count){for(unsigned i=0;i<count;i++)if(!strcmp(s,table[i]))return i;return -1;}
#define LOG_ID(s,table) log_id(s,table,sizeof(table)/sizeof(table[0]))
static bool log_encode(const char *level,const char *module,const char *event,service_record_t *r){
    int l=LOG_ID(level,log_levels),m=LOG_ID(module,log_modules),e=LOG_ID(event,log_events);
    if(l<0||m<0)return false;
    r->level=l;r->module=m;r->argument=0;
    if(e<0 && !strcmp(module,"mqtt")){
        for(unsigned i=0;i<sizeof(log_stages)/sizeof(log_stages[0]);i++){
            size_t n=strlen(log_stages[i]);if(strncmp(event,log_stages[i],n)||strncmp(event+n,"_error_",7))continue;
            char *end;errno=0;long value=strtol(event+n+7,&end,10);if(errno==ERANGE||!event[n+7]||*end||value<INT32_MIN||value>INT32_MAX)return false;
            r->event=128+i;r->argument=value;return true;
        }
    }
    if(e<0)return false;r->event=e;return true;
}
static const char *log_event_text(const service_record_t *r,char out[40]){
    if(r->event<sizeof(log_events)/sizeof(log_events[0]))return log_events[r->event];
    unsigned i=r->event-128;if(i>=sizeof(log_stages)/sizeof(log_stages[0]))return "unknown";
    snprintf(out,40,"%s_error_%ld",log_stages[i],(long)r->argument);return out;
}
