#include "M1/storage.h"
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    m1_display_config_t display;
} baseline_storage_t;
int main(void){
 assert(sizeof(baseline_storage_t)==sizeof(m1_storage_v11_t));
 assert(offsetof(app_storage_t,local_auth)==sizeof(baseline_storage_t));
 assert(offsetof(app_storage_t,base)==offsetof(baseline_storage_t,base));
 assert(offsetof(app_storage_t,features)==offsetof(baseline_storage_t,features));
 assert(offsetof(app_storage_t,automation)==offsetof(baseline_storage_t,automation));
 assert(offsetof(app_storage_t,reserved_controller_config)==offsetof(baseline_storage_t,reserved_controller_config));
 assert(offsetof(app_storage_t,co2)==offsetof(baseline_storage_t,co2));
 assert(offsetof(app_storage_t,co2_source)==offsetof(baseline_storage_t,co2_source));
 assert(offsetof(app_storage_t,display)==offsetof(baseline_storage_t,display));
 baseline_storage_t before;app_storage_t current;
 memset(&before,0xa5,sizeof(before));memset(&current,0x5a,sizeof(current));
 memcpy(&current,&before,sizeof(before));assert(!memcmp(&current,&before,sizeof(before)));
 ph_auth_store trailer;memset(&trailer,0,sizeof(trailer));trailer.magic=PH_AUTH_STORE_MAGIC;trailer.version=PH_AUTH_STORE_VERSION;
 current.local_auth=trailer;assert(!memcmp(&current,&before,sizeof(before)));
 memset(&current.local_auth,0xff,sizeof(current.local_auth));assert(!memcmp(&current,&before,sizeof(before)));
 return 0;
}
