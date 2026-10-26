/* Real device metadata worker and service parser; only transport/job/Flash
 * boundaries are mocked. No DUT, network or production keys are involved. */
#include "mico.h"
#include "../common/generated/build_version.h"
static const char *test_local_version;
#undef FIRMWARE_VERSION
#define FIRMWARE_VERSION test_local_version
#define PHICOMM_DEVICE_OTA 1
static void mico_rtos_delete_thread(void *p){(void)p;}
#include "../common/service.c"
uint32_t test_now;
static bool claimed;
static const char *remote_version;
static unsigned fetches,installs;
void ota_job_init(void){claimed=false;}
bool ota_job_claim(void){if(claimed)return false;claimed=true;return true;}
bool ota_job_enter(void){return claimed;}
void ota_job_release(void){assert(claimed);claimed=false;}
bool ota_job_active(void){return claimed;}
bool ota_job_cancel(void){return false;}
json_object *ota_job_status(void){abort();return NULL;}
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h){(void)a;(void)p;(void)c;*h=false;return NULL;}
json_object *service_product_capabilities(void){json_object *o=json_object_new_object();json_object_object_add(o,"actions",json_object_new_array());return o;}
json_object *service_product(const char *a,json_object *p,const char **code)
{
    assert(!strcmp(a,"ota.start"));
    assert(!strcmp(json_object_get_string(json_object_object_get(p,"url")),
        PHICOMM_OTA_GITHUB_BASE_URL "/releases/v1.0.0/fw.bin.signed"));
    installs++;*code="accepted";return json_object_new_object();
}
bool ota_fetch(const char *url,uint32_t limit,ota_fetch_begin begin,ota_fetch_write write,void *ctx,const char **error)
{
    (void)error;assert(limit==4096 && strstr(url,"/ota/v1/m1/full/latest.json"));fetches++;
    char body[512];int n=snprintf(body,sizeof(body),
        "{\"model\":\"m1\",\"firmware_variant\":\"full\",\"version\":\"%s\","
        "\"path\":\"/releases/v1.0.0/fw.bin.signed\",\"release_log\":\"/releases/v1.0.0/notes.md\"}",remote_version);
    assert(n>0 && n<(int)sizeof(body));
    return begin(ctx,(uint32_t)n) && write(ctx,0,(const uint8_t *)body,(size_t)n);
}

int main(void)
{
    static const struct {const char *local,*remote,*expected;} cases[]={
        {"1.0.0-rc-abcdef","1.0.0-123456","available"},
        {"1.0.0-abcdef","1.0.0-rc-123456","up_to_date"},
        {"1.0.0-rc.2-abcdef","1.0.0-rc.10-123456","available"},
        {"1.0.0-rc.10-abcdef","1.0.0-rc.2-123456","up_to_date"},
        {"1.0.0-rc-abcdef","1.0.0-rc.0","available"},
        {"1.0.0-abcdef-dirty","1.0.0+metadata.1","up_to_date"},
        {"1.0.0-rc.2+old","1.0.0-rc.2+new","up_to_date"},
        {"0.9.27-abcdef","1.0.0-rc-123456","available"},
        {"1.0.0-rc-abcdef","0.9.27-123456","up_to_date"},
        {"1.0.1-abcdef","1.0.0","up_to_date"},
        {"1.0.0-rc-abcdef","1.0.0-rc.01","failed"},
        {"1.0.0-rc-abcdef","1.0.0-rc-garbage","failed"},
        {"1.0.0-rc-abcdef","9.0.0-beta","failed"},
        {"1.0.0-rc-abcdef","01.0.0","failed"},
        {"1.0.0-rc-abcdef","4294967296.0.0","failed"},
        {"1.0.0-unknown","2.0.0","failed"},
    };
    service_init();
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        test_local_version=cases[i].local;remote_version=cases[i].remote;
        assert(ota_job_claim());update_busy=true;update_notes_job=false;update_source=0;
        update_available=false;update_version[0]=update_path[0]=update_notes[0]=0;
        strcpy(update_state,"checking");unsigned before=fetches;
        update_worker(0);
        if(strcmp(update_state,cases[i].expected)) {
            fprintf(stderr,"%s -> %s: expected %s got %s\n",cases[i].local,cases[i].remote,cases[i].expected,update_state);
            abort();
        }
        assert(!update_busy && !claimed && fetches==before+1);
        assert(update_available==!strcmp(cases[i].expected,"available"));
        if(!strcmp(cases[i].expected,"failed"))assert(!update_version[0] && !update_path[0]);
        else assert(!strcmp(update_version,cases[i].remote));
        const char *code=NULL;bool handled=false;unsigned started=installs;
        json_object *params=json_object_new_object();
        json_object *reply=update_command("ota.install",params,&code,&handled);
        assert(reply && handled);
        bool available=!strcmp(cases[i].expected,"available");
        if(strcmp(code,available?"accepted":"invalid_params")) {
            fprintf(stderr,"%s -> %s: install expected %s got %s\n",cases[i].local,cases[i].remote,
                available?"accepted":"invalid_params",code);
            abort();
        }
        assert(installs==started+(unsigned)available);
        json_object_put(reply);json_object_put(params);
    }
    puts("Production device OTA worker/install: rc promotion, ordering, metadata, strict versions and downgrade rejection PASS");
    return 0;
}
#include "stubs/status_wire.inc"
