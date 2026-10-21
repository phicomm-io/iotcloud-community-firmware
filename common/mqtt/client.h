/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_CLIENT_H
#define PH_MQTT_CLIENT_H
#include "MQTTClient.h"
#include "core.h"
#include "owner.h"
#include "../mqtt_account.h"
/* One shared implementation with separate owner contexts. Public producer APIs
 * always target the primary connection; Bemfa uses its explicit private API.
 * Protocol operations and subscription strings stay on their bound RTOS owner. */
/* Creator publishes only after the RTOS has returned the output handle.
 * Generation prevents a previous start from releasing a later worker. */
typedef struct {
    uint32_t state;
}

ph_mqtt_startup;
uint32_t ph_mqtt_startup_prepare(ph_mqtt_startup *);
void ph_mqtt_startup_publish(ph_mqtt_startup *, uint32_t, bool);
bool ph_mqtt_startup_wait(ph_mqtt_startup *, uint32_t);
bool ph_mqtt_owner_init(const mqtt_account_t *, size_t, mico_thread_t *, bool(*accept)(MessageData *, messageHandler));
int ph_mqtt_client_init(Client *, Network *, unsigned);
int ph_mqtt_client_deinit(Client *);
int ph_mqtt_connect(Client *, MQTTPacket_connectData *);
int ph_mqtt_disconnect(Client *);
int ph_mqtt_subscribe(Client *, const char *, enum QoS, messageHandler);
int ph_mqtt_yield(Client *, int);
int ph_mqtt_publish(Client *, const char *, MQTTMessage *);
/* Transfers one already encoded receipt to the owner. An owned wire is retained
 * without a copy; a borrowed busy wire fits the fixed failure buffer. The
 * existing command credit remains held until the actual send or teardown. */
int ph_mqtt_receipt_send(const mqtt_target_t *, const char *, void *, void(*)(void *));
/* Telemetry turn only; command receipts are sent after this scope unwinds. */
void ph_mqtt_turn_begin(unsigned);
void ph_mqtt_turn_end(void);
/* External producers enqueue only. Owner runs a bounded batch after protocol
 * unwinding. Commands have priority, but never recurse through a sender. */
int ph_mqtt_enqueue_send(const char *, const void *, size_t, unsigned, bool);
int ph_mqtt_poll(Client *);
/* Owner only, between protocol/dispatch calls. Queued work or emergency replies;
 * a false result is a snapshot, not a wakeup/producer synchronization promise. */
bool ph_mqtt_work_pending(void);
/* Modern command only: after parsing/copying all borrowed input, consume its
 * queue entry and reserve the maximum result before executing any mutation. */
bool ph_mqtt_input_consumed(void);
void ph_mqtt_outgoing(int(*)(const ph_mqtt_item *));
bool ph_mqtt_owner(void);
void ph_mqtt_owner_end(void);
void ph_mqtt_pending_clear(void);
bool ph_mqtt_topic_matches(const char *, const char *, size_t);
const ph_mqtt_core *ph_mqtt_debug_core(void);

void ph_mqtt_error(unsigned, int);
/* Mark only an immediately returned fatal application error; no extra event,
 * allocation, or cause claim. Successful protocol begin/end and session begin
 * clear the transient marker. The session summary captures it once. */
#define PH_MQTT_APP_SOURCES 1
int ph_mqtt_app_error(unsigned, int);
/* Owner only; reset the latch before Network initialization, not on success. */
void ph_mqtt_session_begin(void);
/* Count a lifecycle outcome without replacing a recorded failure cause. */
void ph_mqtt_lifecycle_event(unsigned, int);
void ph_mqtt_reconnect(void);
void ph_mqtt_snapshot(ph_mqtt_diagnostics *);
#endif
