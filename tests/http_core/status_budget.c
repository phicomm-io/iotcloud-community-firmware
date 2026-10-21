/* ARM32 status-budget fixture; Python inserts unchanged production functions. */

#include "A1/a1.h"
#include "common/httpd/product.h"
#include "common/firmware_variant.h"
#include "common/runtime_metrics.h"
#include <assert.h>
#include <reent.h>
int _write(int,const void*,int);
static a1_config_t cfg; static a1_state_t state;
char a1_id[13]="aabbccddeeff";
uint32_t mico_rtos_get_time(void){return 25000;}
int get_last_reset_reason(void){return 0x30;}
void a1_config_get(a1_config_t *p){*p=cfg;}
void a1_state_get(a1_state_t *p){*p=state;}
void feature_effective_fan(bool on,unsigned speed,bool *eo,unsigned *es){*eo=on;*es=speed;}
bool feature_fan_auto(void){return false;}
unsigned feature_fan_limit(void){return 100;}
/* PRODUCTION_WATCHDOG_FILTER */
#define HTTP_PENDING_CLIENTS 1
#define HTTP_TCP_CLIENT_LIMIT 3
typedef ph_http_diagnostics http_diag_t;
static http_diag_t http_diag_snapshot(void){return (http_diag_t){0};}
static struct {void *p;size_t n;} ledger[2048];
static size_t libc_baseline;
static size_t live,peak,capacity=100000,fail_size,phase,fail_phase,peak_phase;

static unsigned native_depth,native_reentrant_allocations;
struct _reent;
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);void *__real_realloc(void*,size_t);void __real_free(void*);
void *__real__malloc_r(struct _reent*,size_t);void *__real__calloc_r(struct _reent*,size_t,size_t);
void *__real__realloc_r(struct _reent*,void*,size_t);void __real__free_r(struct _reent*,void*);
static size_t charge(size_t n){return ((n+7)&~7u)+8;}
static unsigned find(void*p){for(unsigned i=0;i<2048;i++)if(ledger[i].p==p)return i;abort();}
static void add(void*p,size_t n){if(p){unsigned i=find(NULL);ledger[i].p=p;ledger[i].n=charge(n);live+=charge(n);if(live>peak){peak=live;peak_phase=phase;}}}
static void remove_block(void*p){if(p){unsigned i=find(p);live-=ledger[i].n;ledger[i].p=NULL;}}
static size_t resizing(void*p,size_t n){size_t old=p?ledger[find(p)].n:0;if(live+charge(n)>peak){peak=live+charge(n);peak_phase=phase;}return old;}
static void resized(void*p,void*q,size_t n,size_t old){if(q){if(p){unsigned i=find(p);live-=old;ledger[i].p=NULL;}add(q,n);}}
/* Record only the outermost native allocator operation. Normal malloc calls
 * _malloc_r internally; both must be intercepted without double accounting. */
void *__wrap_malloc(size_t n){bool outer=!native_depth++;void*p=__real_malloc(n);--native_depth;if(outer)add(p,n);return p;}
void *__wrap_calloc(size_t n,size_t m){bool outer=!native_depth++;void*p=__real_calloc(n,m);--native_depth;if(outer)add(p,n*m);return p;}
void *__wrap__malloc_r(struct _reent*r,size_t n){bool outer=!native_depth++;void*p=__real__malloc_r(r,n);--native_depth;if(outer){add(p,n);native_reentrant_allocations++;}return p;}
void *__wrap__calloc_r(struct _reent*r,size_t n,size_t m){bool outer=!native_depth++;void*p=__real__calloc_r(r,n,m);--native_depth;if(outer){add(p,n*m);native_reentrant_allocations++;}return p;}
void *__wrap_realloc(void*p,size_t n){bool outer=!native_depth++;size_t old=outer?resizing(p,n):0;void*q=__real_realloc(p,n);--native_depth;if(outer)resized(p,q,n,old);return q;}
void *__wrap__realloc_r(struct _reent*r,void*p,size_t n){bool outer=!native_depth++;size_t old=outer?resizing(p,n):0;void*q=__real__realloc_r(r,p,n);--native_depth;if(outer)resized(p,q,n,old);return q;}
void __wrap_free(void*p){bool outer=!native_depth++;if(outer)remove_block(p);__real_free(p);--native_depth;}
void __wrap__free_r(struct _reent*r,void*p){bool outer=!native_depth++;if(outer)remove_block(p);__real__free_r(r,p);--native_depth;}
char *__wrap_strdup(const char*s){size_t n=strlen(s)+1;void*p=__wrap_malloc(n);if(p)memcpy(p,s,n);return p;}
int json_heap_permit(size_t n){int ok=live+16384+32<=capacity && n<=capacity-live-16384-32;if(!ok&&!fail_size){fail_size=n;fail_phase=phase;}return ok;}
micoMemInfo_t *MicoGetMemoryInfo(void){static micoMemInfo_t m;m.free_memory=capacity-live;m.allocted_memory=live;m.num_of_chunks=3;return &m;}
json_object *service_parse(const void*s,size_t n){(void)n;return json_tokener_parse(s);}
unsigned service_memory_claim(size_t n){return service_memory_cost(n);}
void service_memory_release(unsigned n){(void)n;}
void service_memory_waiting(bool b){(void)b;}
uint64_t service_memory_failures(void){json_heap_stats_t s;json_heap_snapshot(&s);return s.guard_denied+s.allocation_failures;}

/* PRODUCTION_BUILDERS */
/* PRODUCTION_RESULT_QUERY */
static void healthy_stats(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);assert(live==libc_baseline);}
static void setup(unsigned longest){
 memset(&cfg,0,sizeof(cfg));memset(&state,0,sizeof(state));
 strcpy(cfg.name,"test A1");strcpy(cfg.ssid,"test ssid");strcpy(cfg.mqtt_host,"mqtt.example.com");strcpy(cfg.mqtt_user,"testuser");
 strcpy(state.ip,"192.168.1.111");strcpy(state.wifi_trial_status,"idle");strcpy(state.ap_name,"Phicomm_A1");strcpy(state.ota_state,"idle");
 if(longest){
#define FILL(x) memset(x,'X',sizeof(x)-1)
 FILL(cfg.name);FILL(cfg.ssid);FILL(cfg.mqtt_host);FILL(cfg.mqtt_user);
 FILL(state.mqtt_last_error);FILL(state.wifi_trial_status);FILL(state.ip);FILL(state.ap_name);
 FILL(state.ota_state);FILL(state.ota_error);FILL(state.save_error);FILL(state.ota_release_counter);FILL(state.ota_version);
#undef FILL
 }
 if(longest==2){
#define ESC(x) for(unsigned i=0;i<sizeof(x)-1;i++)x[i]="\"\\/"[i%3]
 ESC(cfg.name);ESC(cfg.ssid);ESC(cfg.mqtt_user);
 ESC(state.mqtt_last_error);ESC(state.ota_error);ESC(state.save_error);
#undef ESC
 }
}
static void measure(bool modern,unsigned longest,bool post,size_t budget,bool expect_ok){
 setup(longest);capacity=budget+libc_baseline;peak=fail_size=fail_phase=0;phase=1;
 json_object *request=post?json_tokener_parse("{\"action\":\"status.get\",\"params\":{\"view\":\"full\"},\"request_id\":\"0123456789012345678901234567890123456789012345678901234567890123\"}"):NULL;
 phase=2;json_object *response=post?service_command(request):service_query("status.get");
 json_object_put(request);request=NULL;
 size_t graph=live-libc_baseline;phase=4;
 ph_http_product p={0};ph_http_exchange x={0};struct printbuf *wire;
 if(modern){ph_http_json(&p,&x,response);wire=p.wire;}else{wire=json_object_to_json_string_ex(response);json_object_put(response);}
 assert((wire!=NULL)==expect_ok);
 size_t output=wire?(size_t)wire->bpos:0;
 if(wire){assert(strstr(wire->buf,"\"model\": \"a1\""));assert(strstr(wire->buf,"http_core_close_failures"));}
 if(modern)ph_http_product_release(&p);else printbuf_free(wire);
 json_object_put(request);healthy_stats();
 char out[256];int n=snprintf(out,sizeof(out),"%s %s %s capacity=%u graph=%u peak=%u@%u denied=%u phase=%u bytes=%u live=%u\n",modern?"consume":"legacy",longest==2?"escaped-text":longest?"maxstrings":"ordinary",post?"POST":"GET",(unsigned)budget,(unsigned)graph,(unsigned)(peak-libc_baseline),(unsigned)peak_phase,(unsigned)fail_size,(unsigned)fail_phase,(unsigned)output,(unsigned)(live-libc_baseline));_write(1,out,n);
}
int main(void){
 /* Warm up newlib's lazily initialized float conversion before measuring. */
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 struct _reent context;_REENT_INIT_PTR(&context);
 size_t before=live;void *direct=_malloc_r(&context,123);assert(direct&&live>=before+charge(123));_free_r(&context,direct);assert(live==before);
 unsigned counted=native_reentrant_allocations;double number;
 assert(_sscanf_r(&context,"1.234567890123456789e20","%lg",&number)==1);
 _snprintf_r(&context,warm,sizeof(warm),"%g",number);_reclaim_reent(&context);
 assert(native_reentrant_allocations>counted);healthy_stats();
 measure(false,false,false,100000,true);
 measure(false,false,false,29116,false);
 measure(true,false,false,29116,true);
 measure(true,false,true,29116,true);
 measure(true,true,false,29116,true);
 measure(true,true,true,29116,true);
 /* More than 1 KiB below observed mature A1 free heap, ordinary full still fits. */
 measure(true,false,true,28000,true);
 measure(true,2,false,29116,true);
 measure(true,2,true,29116,true);
 /* Stronger pressure remains fail-closed, and does not poison recovery. */
 measure(true,true,true,22000,false);
 measure(true,true,true,29116,true);
 return 0;
}
