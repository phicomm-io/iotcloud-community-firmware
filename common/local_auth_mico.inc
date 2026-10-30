/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One product-main TU. Lock order is auth -> product configuration, never reverse.
 * Product defines AUTH_CONFIG_LOCK, AUTH_STORE, AUTH_PASSWORD, AUTH_CONTEXT. */
#include "local_auth.c"
static ph_auth_state local_auth;
static mico_mutex_t local_auth_mutex;
static bool local_auth_ready;
static uint32_t local_auth_now(void *user){(void)user;return mico_rtos_get_time();}
static bool local_auth_random(void *user,uint8_t *out,size_t n)
{(void)user;return MicoRandomNumberRead(out,n)==kNoErr;}
static void local_auth_lock(void *user){(void)user;mico_rtos_lock_mutex(&local_auth_mutex);}
static void local_auth_unlock(void *user){(void)user;mico_rtos_unlock_mutex(&local_auth_mutex);}
static bool local_auth_password(void *user,char out[33])
{
    (void)user;mico_rtos_lock_mutex(&AUTH_CONFIG_LOCK);
    memcpy(out,AUTH_PASSWORD,33);mico_rtos_unlock_mutex(&AUTH_CONFIG_LOCK);
    return memchr(out,0,33)!=NULL;
}
static bool local_auth_save(void *user,const ph_auth_store *next)
{
    (void)user;if(device_config_recovery())return false;
#ifdef AUTH_SAVE_RESERVE
    if(!AUTH_SAVE_RESERVE())return false;
#endif
    mico_rtos_lock_mutex(&AUTH_CONFIG_LOCK);
    /* Core retains the old store until this synchronous save succeeds. */
    AUTH_STORE=*next;
    bool ok=!device_config_recovery()&&mico_system_context_update(AUTH_CONTEXT)==kNoErr;
    if(!ok)AUTH_STORE=local_auth.store;
    mico_rtos_unlock_mutex(&AUTH_CONFIG_LOCK);
#ifdef AUTH_SAVE_RELEASE
    AUTH_SAVE_RELEASE();
#endif
    return ok;
}
ph_auth_state *device_local_auth(void){return local_auth_ready?&local_auth:NULL;}
bool device_local_auth_init(void)
{
    if(local_auth_ready)return true;
#ifdef AUTH_INIT_CONFIG
    if(!AUTH_INIT_CONFIG())return false;
#endif
    if(mico_rtos_init_mutex(&local_auth_mutex)!=kNoErr)return false;
    local_auth.store=AUTH_STORE;
    /* The old SDK CRC covers the entire partition, including the erased tail.
     * An old prefix can therefore be retained without memcpy into a new layout.
     * Unknown/newer trailer schemas are preserved in recovery, never reset. */
    if(!ph_auth_store_valid(&AUTH_STORE) && ph_auth_store_migrate_legacy(&AUTH_STORE)){
        /* The SDK checked the old partition CRC, including the erased tail.
         * Only the known extension changes. Retain a usable read-only state if
         * persistence fails; never turn a failed migration into a boot loop. */
        local_auth.store=AUTH_STORE;
        if(!device_config_recovery()&&!local_auth_save(NULL,&AUTH_STORE))
            device_config_recovery_set(true);
    }
    if(!ph_auth_store_valid(&AUTH_STORE)){
        const uint8_t *p=(const uint8_t *)&AUTH_STORE;bool erased=true,zero=true;
        for(size_t i=0;i<sizeof(AUTH_STORE);i++){erased=erased&&p[i]==255;zero=zero&&p[i]==0;}
        if(!erased&&!zero){
            device_config_recovery_set(true);
            ph_auth_ops ops={NULL,local_auth_now,local_auth_random,local_auth_lock,
                            local_auth_unlock,local_auth_password,local_auth_save};
            local_auth_ready=ph_auth_init(&local_auth,&ops,NULL);return local_auth_ready;
        }
        const ph_auth_store *empty=ph_auth_store_empty_value();
        if(device_config_recovery())AUTH_STORE=*empty;
        else if(!local_auth_save(NULL,empty))return false;
    }
    ph_auth_ops ops={NULL,local_auth_now,local_auth_random,local_auth_lock,
                     local_auth_unlock,local_auth_password,local_auth_save};
    local_auth_ready=ph_auth_init(&local_auth,&ops,&AUTH_STORE);return local_auth_ready;
}
void device_local_auth_revoke(void)
{if(local_auth_ready)ph_auth_local_revoke(&local_auth,&AUTH_STORE);}
#include "lan_auth.inc"
