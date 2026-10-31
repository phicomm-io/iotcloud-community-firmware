/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "os.h"
#include "device.h"
#ifdef PH_HOST_TEST
#include "platform_host.h"
#else
#include "../../new_cfg.h"
#include "../../hal/hal_wifi.h"
#include "../../hal/hal_ota.h"
#endif
#include "../protocol/mqtt_account.h"
extern ph_device bl_device;
extern ph_auth_state bl_auth;
extern volatile bool bl_ready;
extern unsigned bl_mqtt_status;
/* Protected by bl_lock. Quiescent is published only after session deinit. */
extern bool bl_ota_pending, bl_mqtt_quiescent;
void bl_lock(void);
void bl_unlock(void);
void bl_mqtt_start(void);
void bl_mqtt_diagnostics(ph_writer *);
/* Broker adapter only; caller holds bl_lock and supplies the validated boot. */
bool bl_sensor_grant(const char *peer,const char *token,uint32_t boot);
size_t bl_error(void *,size_t,ph_wire_format,const char *);
