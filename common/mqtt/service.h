/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../service_mp.h"
#include "owner.h"
#include "../mqtt_account.h"
typedef int (*service_mqtt_sender)(const char *,const void *,size_t,bool);
int service_mqtt_input(service_mqtt_sender,const mqtt_account_t *,const void *,size_t,bool);
int service_mqtt_flush(service_mqtt_sender);
int service_mqtt_snapshot(service_mqtt_sender,const mqtt_account_t *);
int service_mqtt_capabilities(service_mqtt_sender,const mqtt_account_t *);
void service_mqtt_clear(void);
