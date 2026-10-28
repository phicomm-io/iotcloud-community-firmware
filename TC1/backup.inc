/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../common/backup.h"
#include "../common/backup_validation.h"
#include "../common/firmware_variant.h"
typedef struct {
    user_config_v12_t app;
    char name[maxNameLen],ssid[33],password[65];
} tc1_backup_storage;
typedef struct { volatile int state; } backup_job;
static void backup_reboot(mico_thread_arg_t arg) {
    backup_job *job=(void *)arg;
    while(!job->state)mico_rtos_thread_msleep(10);
    bool committed=job->state>0;free(job);
    if(committed){mico_rtos_thread_msleep(1500);MicoSystemReboot();}
    mico_rtos_delete_thread(NULL);
}
size_t device_backup_size(void) {return sizeof(ph_backup_header)+sizeof(tc1_backup_storage);}
bool device_backup_export(void *out,size_t capacity) {
    if(capacity!=device_backup_size() || device_config_recovery() || !tc1_config_reserve())return false;
    tc1_backup_storage *snapshot=(void *)((uint8_t *)out+sizeof(ph_backup_header));memset(snapshot,0,sizeof(*snapshot));
    tc1_status_lock();memcpy(&snapshot->app,user_config,sizeof(snapshot->app));tc1_status_unlock();
    /* Obsolete linked-list addresses are never exported or restored. Current
     * schedules and button maps live in the pointer-free automation extension. */
    snapshot->app.task_top=NULL;snapshot->app.task_count=0;memset(snapshot->app.timed_tasks,0,sizeof(snapshot->app.timed_tasks));
    snprintf(snapshot->name,sizeof(snapshot->name),"%s",sys_config->micoSystemConfig.name);
    snprintf(snapshot->ssid,sizeof(snapshot->ssid),"%s",sys_config->micoSystemConfig.ssid);
    snprintf(snapshot->password,sizeof(snapshot->password),"%s",sys_config->micoSystemConfig.user_key);
    tc1_config_release();ph_backup_seal(out,"tc1",PHICOMM_FIRMWARE_VARIANT,VERSION,USER_CONFIG_VERSION,sizeof(*snapshot));return true;
}
typedef struct {const tc1_backup_storage *next;const char *result;} tc1_restore_args;
static bool tc1_restore_commit(void *opaque,const ph_auth_store *store)
{
    tc1_restore_args *args=opaque;const tc1_backup_storage *next=args->next;
    if(device_config_recovery() || !tc1_config_reserve()){args->result="busy";return false;}
    user_config_t *before=malloc(sizeof(*before));if(!before){tc1_config_release();args->result="busy";return false;}
    backup_job *job=calloc(1,sizeof(*job));
    if(!job || mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"restore",backup_reboot,1024,(mico_thread_arg_t)job)!=kNoErr){free(job);free(before);tc1_config_release();args->result="busy";return false;}
    *before=*user_config;__typeof__(sys_config->micoSystemConfig) previous=sys_config->micoSystemConfig;
    tc1_status_lock();memcpy(user_config,&next->app,sizeof(next->app));user_config->local_auth=*store;tc1_status_unlock();
    snprintf(sys_config->micoSystemConfig.name,sizeof(sys_config->micoSystemConfig.name),"%s",next->name);
    snprintf(sys_config->micoSystemConfig.ssid,sizeof(sys_config->micoSystemConfig.ssid),"%s",next->ssid);
    snprintf(sys_config->micoSystemConfig.user_key,sizeof(sys_config->micoSystemConfig.user_key),"%s",next->password);
    sys_config->micoSystemConfig.user_keyLength=strlen(next->password);sys_config->micoSystemConfig.configured=allConfigured;
    sys_config->micoSystemConfig.keyLength=0;memset(sys_config->micoSystemConfig.bssid,0,6);
    bool ok=mico_system_context_update(sys_config)==kNoErr;
    if(!ok){tc1_status_lock();*user_config=*before;tc1_status_unlock();sys_config->micoSystemConfig=previous;}
    free(before);
    job->state=ok?1:-1;
    if(!ok)tc1_config_release();args->result=ok?"accepted":"storage_error";return ok;
}
const char *device_backup_import(const void *data,size_t length) {
    const tc1_backup_storage *next=ph_backup_open(data,length,"tc1",PHICOMM_FIRMWARE_VARIANT,USER_CONFIG_VERSION,sizeof(tc1_backup_storage));
    if(!next)return "invalid_backup";
    const user_config_v12_t *c=&next->app;
#define TEXT(field) if(!ph_backup_text(c->field,sizeof(c->field)))return "invalid_backup"
    TEXT(mqtt_host);TEXT(mqtt_user);TEXT(mqtt_password);TEXT(web_password);TEXT(ap_name);TEXT(ap_key);
#undef TEXT
    if(c->version!=USER_CONFIG_VERSION || c->mqtt_port<0 || c->mqtt_port>65535 || c->mqtt_topic_mode>1 || c->mqtt_report_freq<1 || c->mqtt_report_freq>3600 ||
       c->bemfa.language>1 || !bemfa_config_valid(&c->bemfa,true) || c->task_top || c->task_count ||
       !ph_backup_automation_valid(&c->automation) ||
       !ph_backup_text(next->name,sizeof(next->name)) || !ph_backup_text(next->ssid,sizeof(next->ssid)) || !ph_backup_text(next->password,sizeof(next->password)))return "invalid_backup";
    for(unsigned i=0;i<6;i++)if(!ph_backup_text(c->socket_names[i],sizeof(c->socket_names[i])) || c->socket_status[i]>1)return "invalid_backup";
    for(unsigned i=0;i<MAX_TASK_NUM;i++)if(c->timed_tasks[i].next || c->timed_tasks[i].on_use || c->automation.tasks[i].minute>=1440 || c->automation.tasks[i].action.kind>AUTO_ACTION_MAX || c->automation.tasks[i].once>2)return "invalid_backup";
    tc1_restore_args args={next,"busy"};
    if(device_local_auth())ph_auth_credential_commit(device_local_auth(),next->app.web_password,true,tc1_restore_commit,&args);
    return args.result;
}
