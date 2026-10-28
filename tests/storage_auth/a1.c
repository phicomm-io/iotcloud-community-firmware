#include "A1/storage.h"
typedef struct {
    a1_config_t base;
    feature_config_storage_t features;
    automation_storage_t automation;
    a1_filter_storage_t filter;
    automation_action_t combination_buttons[34];
    feature_auto_config_t automatic;
    feature_co2_config_t co2;
    bemfa_config_t bemfa;
    fresh_config_t fresh_air; /* Tagged append-only extension; retained by ordinary A1. */
    a1_filter_stock_t filter_stock; /* v10: after all v9 fields, including fresh-air. */
} baseline_storage_t;
int main(void){
 assert(sizeof(baseline_storage_t)==sizeof(a1_storage_v10_t));
 assert(offsetof(app_storage_t,local_auth)==sizeof(baseline_storage_t));
 assert(offsetof(app_storage_t,base)==offsetof(baseline_storage_t,base));
 assert(offsetof(app_storage_t,features)==offsetof(baseline_storage_t,features));
 assert(offsetof(app_storage_t,automation)==offsetof(baseline_storage_t,automation));
 assert(offsetof(app_storage_t,filter)==offsetof(baseline_storage_t,filter));
 assert(offsetof(app_storage_t,combination_buttons)==offsetof(baseline_storage_t,combination_buttons));
 assert(offsetof(app_storage_t,automatic)==offsetof(baseline_storage_t,automatic));
 assert(offsetof(app_storage_t,co2)==offsetof(baseline_storage_t,co2));
 assert(offsetof(app_storage_t,bemfa)==offsetof(baseline_storage_t,bemfa));
 assert(offsetof(app_storage_t,fresh_air)==offsetof(baseline_storage_t,fresh_air));
 assert(offsetof(app_storage_t,filter_stock)==offsetof(baseline_storage_t,filter_stock));
 baseline_storage_t before;app_storage_t current;
 memset(&before,0xa5,sizeof(before));memset(&current,0x5a,sizeof(current));
 memcpy(&current,&before,sizeof(before));assert(!memcmp(&current,&before,sizeof(before)));
 ph_auth_store trailer;memset(&trailer,0,sizeof(trailer));trailer.magic=PH_AUTH_STORE_MAGIC;trailer.version=PH_AUTH_STORE_VERSION;
 current.local_auth=trailer;assert(!memcmp(&current,&before,sizeof(before)));
 memset(&current.local_auth,0xff,sizeof(current.local_auth));assert(!memcmp(&current,&before,sizeof(before)));
 return 0;
}
