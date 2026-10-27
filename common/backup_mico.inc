/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Product supplies its storage, mutex and restore reservation callbacks. */
#include "backup.h"
#include "backup_validation.h"
#include <math.h>
#include "firmware_variant.h"
static bool backup_linkage_valid(const feature_linkage_t *l){
    if(l->enabled>1||l->mode>2||l->statistic>STAT_FAILBACK||l->fixed_speed>100||l->min_speed>l->max_speed||l->max_speed>100)return false;
    const float values[]={l->on_pm,l->off_pm,l->on_hcho,l->off_hcho,l->target_pm,l->target_hcho,l->low_pm,l->high_pm,l->low_hcho,l->high_hcho,l->kp,l->ki,l->kd};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++)if(!isfinite(values[i]))return false;
    return true;
}
static bool backup_storage_valid(const app_storage_t *next) {
    const BACKUP_BASE *c=&next->base;
    bool version_valid=c->version==BACKUP_VERSION;
#ifndef FEATURE_DEVICE_A1
    /* Read only each legacy version's retained prefix before migration. The
     * display starts inside v8 tail padding; presentation is absent in v9. */
    version_valid=version_valid || c->version==8 || c->version==9 || c->version==10;
#endif
#define BACKUP_TEXT(field) if(!ph_backup_text(c->field,sizeof(c->field)))return false
    BACKUP_TEXT(ssid);BACKUP_TEXT(wifi_password);BACKUP_TEXT(mqtt_host);BACKUP_TEXT(mqtt_user);
    BACKUP_TEXT(mqtt_password);BACKUP_TEXT(name);BACKUP_TEXT(web_password);
#undef BACKUP_TEXT
    const feature_config_storage_t *f=&next->features;
    if(c->magic!=BACKUP_MAGIC || !version_valid || !c->mqtt_port ||
       f->magic!=FEATURE_MAGIC || f->language>PH_LANG_EN || f->peer_count>FEATURE_STORAGE_PEERS || f->member_count>8 || f->event_count>8 || f->sleep_count>4 ||
       !ph_backup_text(f->group,sizeof(f->group)) || !ph_backup_text(f->ntp_host,sizeof(f->ntp_host)) ||
       f->timezone_minutes < -720 || f->timezone_minutes>840 || f->linkage.statistic>STAT_FAILBACK || f->light.effect>EFFECT_COLOR_BREATHE || f->light.segments>4 ||
       next->automation.magic!=AUTOMATION_MAGIC || next->automation.timezone < -720 || next->automation.timezone>840)return false;
    for(unsigned i=0;i<FEATURE_STORAGE_PEERS;i++)if(!ph_backup_text(f->peers[i].id,sizeof(f->peers[i].id)) || !ph_backup_text(f->peers[i].ip,sizeof(f->peers[i].ip)) || !ph_backup_text(f->peers[i].group,sizeof(f->peers[i].group)))return false;
    for(unsigned i=0;i<8;i++)if(!ph_backup_text(f->members[i].id,sizeof(f->members[i].id)) || !ph_backup_text(f->members[i].ip,sizeof(f->members[i].ip)))return false;
    if(!ph_backup_automation_valid((const automation_storage_t *)&next->automation) || !backup_linkage_valid(&f->linkage) || !isfinite(f->light.quality_pm) || !isfinite(f->light.quality_hcho))return false;
    for(unsigned i=0;i<4;i++){
        const feature_segment_t *v=&f->light.segment[i];
        if(v->source>FIELD_FAN || v->display>DISPLAY_BOTH || !isfinite(v->minimum)||!isfinite(v->maximum)||!isfinite(v->suitable_low)||!isfinite(v->suitable_high))return false;
        if(f->sleeps[i].start>=1440||f->sleeps[i].end>=1440||f->sleeps[i].days>127||f->sleeps[i].max_speed>100)return false;
    }
    for(unsigned i=0;i<8;i++)if(f->events[i].minute>=1440||f->events[i].days>127||f->events[i].speed>100)return false;
#ifdef FEATURE_DEVICE_A1
    if(c->speed>100 || c->fan_on>1 || c->led_enabled>1 || c->child_lock>1 || next->filter.magic!=A1_FILTER_MAGIC || !a1_filter_stock_valid(&next->filter_stock) || next->filter.lifetime_hours<1 || next->filter.lifetime_hours>A1_FILTER_MAX_HOURS || !bemfa_config_valid(&next->bemfa,false) || next->automatic.fallback_speed>100)return false;
    for(unsigned i=0;i<34;i++)if(!ph_backup_action_valid(&next->combination_buttons[i]))return false;
    if(next->fresh_air.magic && (next->fresh_air.magic!=FRESH_MAGIC || next->fresh_air.schema!=FRESH_SCHEMA || next->fresh_air.speed>100 || !fresh_mapping_valid(&next->fresh_air.mapping) || !backup_linkage_valid(&next->fresh_air.linkage)))return false;
#else
    if(next->co2.magic!=M1_CO2_MAGIC || next->co2.mode>2 || next->co2.slot>1 || !next->co2.interval || next->co2.interval>3600 || next->co2.low>next->co2.high)return false;
    if(!m1_co2_source_config_valid(&next->co2_source) || (next->co2_source.external&&!strcmp(next->co2_source.device_id,m1_id)))return false;
    if(c->version==9 && !m1_display_v9_valid(&next->display))return false;
    if(c->version==10 && !m1_display_v10_valid(&next->display))return false;
    if(c->version==BACKUP_VERSION && !m1_display_config_valid(&next->display))return false;
    if(c->version>=9)for(unsigned i=0;i<M1_DISPLAY_LANES;i++)
        if(next->display.lanes[i].source==M1_DISPLAY_MQTT && !strcmp(next->display.lanes[i].device_id,m1_id))return false;
    if(c->brightness>4 || c->last_on>4)return false;
#endif
    return true;
}
size_t device_backup_size(void) {return sizeof(ph_backup_header)+sizeof(app_storage_t);}
bool device_backup_export(void *out,size_t capacity) {
    if(capacity!=device_backup_size() || device_config_recovery())return false;
    mico_rtos_lock_mutex(&BACKUP_LOCK);
    memcpy((uint8_t *)out+sizeof(ph_backup_header),BACKUP_LIVE,sizeof(app_storage_t));
    mico_rtos_unlock_mutex(&BACKUP_LOCK);
    ph_backup_seal(out,BACKUP_MODEL,PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,BACKUP_VERSION,sizeof(app_storage_t));return true;
}
const char *device_backup_import(const void *data,size_t length) {
    const app_storage_t *next=ph_backup_open(data,length,BACKUP_MODEL,PHICOMM_FIRMWARE_VARIANT,BACKUP_VERSION,sizeof(app_storage_t));
    if(next && next->base.version!=BACKUP_VERSION)return "invalid_backup";
    size_t restore_size=sizeof(app_storage_t);
#ifndef FEATURE_DEVICE_A1
    unsigned restore_version=BACKUP_VERSION;
    if(!next){
        restore_size=sizeof(m1_storage_v10_t);restore_version=10;
        next=ph_backup_open(data,length,BACKUP_MODEL,PHICOMM_FIRMWARE_VARIANT,10,restore_size);
        if(next && next->base.version!=10)return "invalid_backup";
    }
    if(!next){
        restore_size=sizeof(m1_storage_v9_t);restore_version=9;
        next=ph_backup_open(data,length,BACKUP_MODEL,PHICOMM_FIRMWARE_VARIANT,9,restore_size);
        if(next && next->base.version!=9)return "invalid_backup";
    }
    if(!next){
        restore_size=sizeof(m1_storage_v8_t);restore_version=8;
        next=ph_backup_open(data,length,BACKUP_MODEL,PHICOMM_FIRMWARE_VARIANT,8,restore_size);
        if(next && next->base.version!=8)return "invalid_backup";
    }
#endif
    if(!next || !backup_storage_valid(next))return "invalid_backup";
    app_storage_t *before=malloc(sizeof(*before));if(!before)return "busy";
    mico_rtos_lock_mutex(&BACKUP_LOCK);
    if(device_config_recovery() || !backup_reserve()){mico_rtos_unlock_mutex(&BACKUP_LOCK);free(before);return "busy";}
    memcpy(before,BACKUP_LIVE,sizeof(*before));memcpy(BACKUP_LIVE,next,restore_size);
#ifndef FEATURE_DEVICE_A1
    if(restore_version==8){
        m1_display_defaults(&((app_storage_t *)BACKUP_LIVE)->display);
    }
    if(restore_version==9)m1_display_upgrade_v9(&((app_storage_t *)BACKUP_LIVE)->display);
    if(restore_version==9 || restore_version==10)m1_display_upgrade_remote(&((app_storage_t *)BACKUP_LIVE)->display);
    BACKUP_LIVE->version=BACKUP_VERSION;
#endif
    BACKUP_LIVE->revision=before->base.revision+1;
    bool ok=mico_system_context_update(BACKUP_CONTEXT)==kNoErr;
    if(!ok)memcpy(BACKUP_LIVE,before,sizeof(*before));
    backup_finish(ok);mico_rtos_unlock_mutex(&BACKUP_LOCK);free(before);
    return ok?"accepted":"storage_error";
}
