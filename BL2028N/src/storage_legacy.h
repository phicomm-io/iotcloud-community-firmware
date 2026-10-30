/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* Frozen GCC5 ARM EABI disk types from c11e58dd. Do not use current auth,
 * settings, slot-count macros or packed types when decoding these records. */
#define PH_STORE_V1_MAGIC UINT32_C(0x314c4250)
#define PH_STORE_V2_MAGIC UINT32_C(0x324c4250)
typedef struct {
    char peer[13];uint8_t key[16],scopes,reserved[2];
} ph_store_old_pair;
typedef struct {
    uint32_t magic;uint16_t version,count;ph_store_old_pair pairs[12];
} ph_store_old_auth;
typedef struct {
    uint32_t magic,revision;uint8_t child_lock,indicator,power_on,calibrated;
    double coefficients[3],energy_kwh;ph_store_old_auth auth;
} ph_store_v1_settings;
typedef struct {
    uint32_t magic,revision;uint8_t child_lock,indicator,power_on,calibrated;
    double coefficients[3],energy_kwh;ph_store_old_auth auth;
    uint32_t report_seconds;char name[64],socket_name[64];
} ph_store_v2_settings;
typedef struct {uint32_t magic,size,crc;ph_store_v1_settings settings;} ph_store_v1_record;
typedef struct {uint32_t magic,size,crc;ph_store_v2_settings settings;} ph_store_v2_record;
#define PH_STORE_ABI_ASSERT(name,expression) typedef char ph_store_abi_##name[(expression)?1:-1]
PH_STORE_ABI_ASSERT(pair,sizeof(ph_store_old_pair)==32);
PH_STORE_ABI_ASSERT(auth,sizeof(ph_store_old_auth)==392);
PH_STORE_ABI_ASSERT(v1,sizeof(ph_store_v1_settings)==440);
PH_STORE_ABI_ASSERT(v1_record,sizeof(ph_store_v1_record)==456);
PH_STORE_ABI_ASSERT(v2,sizeof(ph_store_v2_settings)==576);
PH_STORE_ABI_ASSERT(v2_record,sizeof(ph_store_v2_record)==592);
PH_STORE_ABI_ASSERT(record_payload,offsetof(ph_store_v2_record,settings)==16);
PH_STORE_ABI_ASSERT(coefficients,offsetof(ph_store_v2_settings,coefficients)==16);
PH_STORE_ABI_ASSERT(energy,offsetof(ph_store_v2_settings,energy_kwh)==40);
PH_STORE_ABI_ASSERT(auth_offset,offsetof(ph_store_v2_settings,auth)==48);
PH_STORE_ABI_ASSERT(report,offsetof(ph_store_v2_settings,report_seconds)==440);
PH_STORE_ABI_ASSERT(name,offsetof(ph_store_v2_settings,name)==444);
PH_STORE_ABI_ASSERT(socket_name,offsetof(ph_store_v2_settings,socket_name)==508);
#undef PH_STORE_ABI_ASSERT
