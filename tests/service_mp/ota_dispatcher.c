/* Execute the real OTA-enabled dispatcher include with minimal surrounding
 * service/OTA backends. No production source is copied or rewritten. Ordinary
 * tests separately link the complete actual service.c translation unit. */
#include "common/service_mp.h"
#include "mico.h"
#include "sha.h"
#include <ctype.h>
#include <string.h>
#define LOG_COUNT 64
static bool ready;
static mico_mutex_t service_lock,command_lock;
#include "common/service_lock.inc"
typedef struct {uint32_t sequence,uptime,utc;uint8_t level,module;} service_record_t;
static service_record_t records[LOG_COUNT];
static uint32_t sequence;
static const char *log_levels[]={"info"},*log_modules[]={"system"};
static const char *log_event_text(const service_record_t *r,char *out){(void)r;(void)out;return "boot";}
static bool identifier(const char *s,size_t max){size_t n=strlen(s);if(!n||n>=max)return false;for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='.'&&s[i]!='-')return false;return true;}
extern bool device_config_recovery(void);
bool test_ota_active;
static unsigned job_status_count;
bool ota_job_active(void){return test_ota_active;}
bool ota_job_cancel(void){bool previous=test_ota_active;test_ota_active=false;return previous;}
void ota_job_status_mp(ph_writer *w){job_status_count++;phw_begin(w,NULL,false);phw_bool(w,"active",test_ota_active);phw_end(w);}
void service_init(void){if(!ready){assert(!mico_rtos_init_mutex(&service_lock));assert(!mico_rtos_init_mutex(&command_lock));assert(service_command_gate_init());ready=true;}}
void service_event(const char *level,const char *module,const char *event){(void)level;(void)module;(void)event;}
static const char *const update_capability_names[]={"ota.status.get","ota.cancel",NULL};
#include "common/service_mp.inc"
static bool update_command_mp(const char *a,const ph_value *p,ph_writer *w,const char **code){(void)a;(void)p;(void)w;(void)code;return false;}
