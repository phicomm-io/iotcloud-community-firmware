/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_SERVICE_H
#define PH_MQTT_SERVICE_H
#include "../service.h"
#include "../mqtt_account.h"
int service_mqtt_send(int (*)(const char *,const char *,bool),const char *,json_object *,bool);
int service_mqtt_input(int (*)(const char *,const char *,bool),const mqtt_account_t *,const void *,size_t,bool);
int service_mqtt_flush(int (*)(const char *,const char *,bool));
int service_mqtt_snapshot(int (*)(const char *,const char *,bool),const mqtt_account_t *);
int service_mqtt_capabilities(int (*)(const char *,const char *,bool),const mqtt_account_t *);
void service_mqtt_clear(void);
#endif
