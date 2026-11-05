
/* Alias the fixture's mutable password, including the empty-password guard. */
static struct {char *web_password;} config_fixture={password},*user_config=&config_fixture;

static int get_last_reset_reason(void){return 1;}
/* TC1 main.c diagnostics: same shape as main.h, no behaviour under test. */
typedef struct {uint32_t reset_reason,button_edges;bool defaults_at_boot,version_mismatch;} tc1_config_diag_t;
static void tc1_config_diag(tc1_config_diag_t*o){o->reset_reason=1;o->button_edges=0;o->defaults_at_boot=false;o->version_mismatch=false;}
static const char *tc1_reset_reason_text(uint32_t reason){(void)reason;return "power_on";}
static void tc1_http_trace_mp(ph_writer*w){phw_begin(w,NULL,false);phw_end(w);}
static bool ota_job_claim(void){if(job)return false;job=true;return true;}
static void ota_job_release(void){assert(job);job=false;}
static bool ota_job_enter(void){assert(job);entered++;return true;}
static bool tc1_ota_reserve(void){return true;}
static bool tc1_ota_begin(uint32_t n,bool rescue){assert(n);ota_begins++;(void)rescue;return true;}
static bool tc1_ota_write(const void*p,size_t n){assert(p&&n<=512);written+=n;return !fail_write;}
static bool tc1_ota_verify(void){finishes++;return true;}
static bool tc1_ota_activate(void){activations++;return !fail_activation;}
static bool tc1_ota_commit_uncertain(void){return activation_uncertain;}
static void tc1_ota_failure(const char*r){assert(r);aborts++;}
static void tc1_ota_abort(void){aborts++;}
static void *mico_system_context_get(void){return NULL;}
#define eState_Software_Reset 1
static void mico_system_power_perform(void*p,int state){(void)p;assert(state==1);reboots++;job=false;}
int ph_http_stop(void){return 0;}
static const unsigned char web_index_html[]={31,139,1,2};

void tc1_config_diagnostics_mp(ph_writer*w){phw_bool(w,"config_recovery",true);phw_u32(w,"config_source_version",12);phw_u32(w,"config_auth_magic",0xffffffffu);phw_u32(w,"config_auth_version",0xffffffffu);}
