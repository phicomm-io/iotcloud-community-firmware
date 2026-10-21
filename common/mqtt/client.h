/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_CLIENT_H
#define PH_MQTT_CLIENT_H
#include "MQTTClient.h"
#include "core.h"
#include "../mqtt_account.h"
/* One compiled instance per firmware. Protocol library and subscription strings
 * are retained; all connection operations require the bound RTOS owner. */
/* Creator publishes only after the RTOS has returned the output handle.
 * Generation prevents a previous start from releasing a later worker. */
typedef struct { volatile uint32_t generation, ready; } ph_mqtt_startup;
uint32_t ph_mqtt_startup_prepare(ph_mqtt_startup *);
void ph_mqtt_startup_publish(ph_mqtt_startup *,uint32_t,bool);
bool ph_mqtt_startup_wait(ph_mqtt_startup *,uint32_t);
bool ph_mqtt_owner_init(const mqtt_account_t *, size_t,mico_thread_t *,
                        bool (*accept)(MessageData *,messageHandler));
int ph_mqtt_client_init(Client *,Network *,unsigned);
int ph_mqtt_client_deinit(Client *);
int ph_mqtt_connect(Client *,MQTTPacket_connectData *);
int ph_mqtt_disconnect(Client *);
int ph_mqtt_subscribe(Client *,const char *,enum QoS,messageHandler);
int ph_mqtt_yield(Client *,int);
int ph_mqtt_publish(Client *,const char *,MQTTMessage *);
enum { PH_MQTT_MORE=2 };
/* Telemetry turn only; command receipts are sent after this scope unwinds. */
void ph_mqtt_turn_begin(unsigned);
void ph_mqtt_turn_end(void);
/* External producers enqueue only. Owner runs a bounded batch after protocol
 * unwinding. Commands have priority, but never recurse through a sender. */
int ph_mqtt_enqueue_send(const char *,const void *,size_t,unsigned,bool);
int ph_mqtt_poll(Client *);
/* Modern command only: after parsing/copying all borrowed input, consume its
 * queue entry and reserve the maximum result before executing any mutation. */
bool ph_mqtt_input_consumed(void);
void ph_mqtt_outgoing(int (*)(const ph_mqtt_item *));
bool ph_mqtt_owner(void);
void ph_mqtt_owner_end(void);
void ph_mqtt_pending_clear(void);
bool ph_mqtt_topic_matches(const char *,const char *,size_t);
const ph_mqtt_core *ph_mqtt_debug_core(void);
enum { PH_MQTT_ERROR_INIT=1,PH_MQTT_ERROR_CONNECT,PH_MQTT_ERROR_SUBSCRIBE,
       PH_MQTT_ERROR_PUBLISH,PH_MQTT_ERROR_YIELD,PH_MQTT_ERROR_SESSION,PH_MQTT_ERROR_NETWORK };
void ph_mqtt_error(unsigned,int);
void ph_mqtt_reconnect(void);
void ph_mqtt_snapshot(ph_mqtt_diagnostics *);
#endif
