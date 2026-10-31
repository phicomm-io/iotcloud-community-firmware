/* Real local-auth core + HTTP adapter; only product persistence is a fixture. */
static ph_auth_state auth_state;
static bool auth_fail_commit;
static unsigned auth_random_calls;
static uint32_t auth_now(void *u){(void)u;return tick;}
static bool auth_random(void *u,uint8_t *out,size_t n){
    (void)u;memset(out,0,n);unsigned value=++auth_random_calls;memcpy(out,&value,sizeof(value));return true;
}
static bool auth_password(void *u,char out[33]){(void)u;memcpy(out,password,33);return true;}
ph_auth_state *device_local_auth(void){return &auth_state;}
static bool auth_commit(void *u,const ph_auth_store *store){
    (void)store;if(auth_fail_commit)return false;strcpy(password,u);return true;
}
static bool credentials(const char *value,bool restore){
    return ph_auth_credential_commit(&auth_state,value,restore,auth_commit,(void *)value);
}
#define FIRMWARE_VERSION "auth-queue-fixture"
#define VERSION FIRMWARE_VERSION
#define PHICOMM_FIRMWARE_VARIANT "full"
static const char a1_id[]="001122334455",m1_id[]="001122334455",str_mac[]="001122334455";
