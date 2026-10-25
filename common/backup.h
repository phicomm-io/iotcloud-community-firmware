/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
/* Little-endian MiCO snapshot, schema 1. Restore only matching model, variant,
 * storage revision and length. Secrets are present: HTTP authentication required.
 * MCU music is stored on a different processor and is not part of this file. */
#define PH_BACKUP_SCHEMA 1u
#define PH_BACKUP_LIMIT 16384u
typedef struct {
    char magic[8], model[8], variant[40], firmware[32];
    uint32_t schema, config_version, length, crc;
} ph_backup_header;
typedef char ph_backup_header_size[sizeof(ph_backup_header)==104?1:-1];
static inline uint32_t ph_backup_crc(const void *data,size_t n) {
    const uint8_t *p=data;uint32_t crc=~0u;
    while(n--){crc^=*p++;for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));}
    return ~crc;
}
static inline bool ph_backup_text(const char *p,size_t n) {return memchr(p,0,n)!=NULL;}
static inline void ph_backup_seal(void *output,const char *model,const char *variant,const char *firmware,unsigned version,size_t bytes) {
    ph_backup_header h={0};memcpy(h.magic,"PHCBACK1",8);
    snprintf(h.model,sizeof(h.model),"%s",model);snprintf(h.variant,sizeof(h.variant),"%s",variant);snprintf(h.firmware,sizeof(h.firmware),"%s",firmware);
    h.schema=PH_BACKUP_SCHEMA;h.config_version=version;h.length=bytes;
    h.crc=ph_backup_crc((const uint8_t *)output+sizeof(h),bytes);memcpy(output,&h,sizeof(h));
}
static inline const void *ph_backup_open(const void *input,size_t size,const char *model,const char *variant,unsigned version,size_t bytes) {
    if(!input || size!=sizeof(ph_backup_header)+bytes || size>PH_BACKUP_LIMIT)return NULL;
    ph_backup_header h;memcpy(&h,input,sizeof(h));
    if(memcmp(h.magic,"PHCBACK1",8) || h.schema!=PH_BACKUP_SCHEMA || h.config_version!=version || h.length!=bytes ||
       !ph_backup_text(h.model,sizeof(h.model)) || !ph_backup_text(h.variant,sizeof(h.variant)) || !ph_backup_text(h.firmware,sizeof(h.firmware)) ||
       strcmp(h.model,model) || strcmp(h.variant,variant))return NULL;
    const uint8_t *payload=(const uint8_t *)input+sizeof(h);
    return h.crc==ph_backup_crc(payload,bytes)?payload:NULL;
}
size_t device_backup_size(void);
bool device_backup_export(void *,size_t);
const char *device_backup_import(const void *,size_t);
bool device_config_recovery(void);
void device_config_recovery_set(bool);
