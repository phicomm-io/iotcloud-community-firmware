/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_SERVICE_H
#define PH_MQTT_SERVICE_H
#include "../service.h"
#include "owner.h"
#include "../mqtt_account.h"
/* Fatal discovery graph publication; encoding failure carries source only. */
int service_mqtt_send(int(*)(const char *, const char *, bool), const char *, json_object *, bool);
int service_mqtt_input(int(*)(const char *, const char *, bool), const mqtt_account_t *, const void *, size_t, bool);
int service_mqtt_flush(int(*)(const char *, const char *, bool));
int service_mqtt_snapshot(int(*)(const char *, const char *, bool), const mqtt_account_t *);
int service_mqtt_capabilities(int(*)(const char *, const char *, bool), const mqtt_account_t *);
void service_mqtt_clear(void);

/* One optional graph attempt attributed to its known MQTT owner. The generic
 * failure scope and both leases end before any transport call. */
typedef struct { unsigned token, claim; } service_mqtt_json_scope;
bool service_mqtt_json_begin(service_mqtt_json_scope *, mico_thread_t *);
void service_mqtt_json_end(service_mqtt_json_scope *);
int service_mqtt_json_error(service_mqtt_json_scope *, unsigned);
int service_mqtt_optional_send(int(*)(const char *,const char *,bool),const char *,json_object *,bool,service_mqtt_json_scope *);

#endif
