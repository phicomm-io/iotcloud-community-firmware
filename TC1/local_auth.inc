/* SPDX-License-Identifier: GPL-3.0-or-later */
static mico_mutex_t tc1_auth_config_lock;
static bool tc1_auth_config_init(void)
{return mico_rtos_init_mutex(&tc1_auth_config_lock)==kNoErr;}
#define AUTH_DEVICE_ID str_mac
#define AUTH_CONFIG_LOCK tc1_auth_config_lock
#define AUTH_STORE (user_config->local_auth)
#define AUTH_PASSWORD (user_config->web_password)
#define AUTH_CONTEXT sys_config
#define AUTH_INIT_CONFIG tc1_auth_config_init
#define AUTH_SAVE_RESERVE tc1_config_reserve
#define AUTH_SAVE_RELEASE tc1_config_release
#include "../common/local_auth_mico.inc"
static bool tc1_auth_password_commit(void *opaque,const ph_auth_store *store)
{
    const char *password=opaque;if(!tc1_config_reserve())return false;
    mico_rtos_lock_mutex(&tc1_auth_config_lock);
    char before[33];memcpy(before,user_config->web_password,33);
    const ph_auth_store *previous=&device_local_auth()->store;
    tc1_status_lock();memset(user_config->web_password,0,33);
    memcpy(user_config->web_password,password,strlen(password));user_config->local_auth=*store;tc1_status_unlock();
    bool ok=!device_config_recovery()&&mico_system_context_update(sys_config)==kNoErr;
    if(!ok){tc1_status_lock();memcpy(user_config->web_password,before,33);user_config->local_auth=*previous;tc1_status_unlock();}
    memset(before,0,sizeof(before));mico_rtos_unlock_mutex(&tc1_auth_config_lock);tc1_config_release();return ok;
}
bool tc1_auth_password_save(const char *password)
{
    if(!device_local_auth()||!password||strlen(password)>32)return false;
    return ph_auth_credential_commit(device_local_auth(),password,false,tc1_auth_password_commit,(void *)password);
}
static bool tc1_auth_reset_commit(void *opaque,const ph_auth_store *store)
{
    bool full=*(bool *)opaque;
    if((!full&&device_config_recovery())||!tc1_config_reserve())return false;
    user_config_t *before=malloc(sizeof(*before));
    if(!before){tc1_config_release();return false;}
    mico_rtos_lock_mutex(&tc1_auth_config_lock);
    tc1_status_lock();*before=*user_config;tc1_status_unlock();
    __typeof__(sys_config->micoSystemConfig) previous=sys_config->micoSystemConfig;
    bool recovery=config_recovery;OSStatus err=kNoErr;
    if(full){config_recovery=false;err=mico_system_context_restore(sys_config);}
    else{
        sys_config->micoSystemConfig.ssid[0]=0;sys_config->micoSystemConfig.user_key[0]=0;
        sys_config->micoSystemConfig.user_keyLength=0;sys_config->micoSystemConfig.key[0]=0;
        sys_config->micoSystemConfig.keyLength=0;user_config->web_password[0]=0;
    }
    user_config->local_auth=*store;
    if(err==kNoErr)err=mico_system_context_update(sys_config);
    if(err!=kNoErr){tc1_status_lock();*user_config=*before;tc1_status_unlock();
        sys_config->micoSystemConfig=previous;config_recovery=recovery||config_commit_uncertain;}
    mico_rtos_unlock_mutex(&tc1_auth_config_lock);free(before);
    if(err!=kNoErr)tc1_config_release();
    /* On success retain the reservation through the caller's reboot. */
    return err==kNoErr;
}
bool tc1_auth_reset(bool full)
{
    if(!device_local_auth())return false;
    return ph_auth_credential_commit(device_local_auth(),"",true,tc1_auth_reset_commit,&full);
}
