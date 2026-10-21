/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "client.h"
#include "queue.h"
bool bm_transport_allocate(mico_thread_t *);
void bm_transport_release(void);
bool bm_owner(void);
mico_thread_t *bm_owner_thread(void);
bool bm_receipt_pending(void);
void bm_owner_end(void);
const ph_mqtt_core *bm_debug_core(void);
int bm_app_error(unsigned stage, int rc);
void bm_error(unsigned stage, int rc);
void bm_session_begin(void);
void bm_lifecycle_event(unsigned stage, int rc);
void bm_reconnect(void);
void bm_snapshot(ph_mqtt_diagnostics *out);
void bm_outgoing(int(*handler)(const ph_mqtt_item *));
void bm_pending_clear(void);
/* NULL account selects plain topics without TMCP emergency receipts. */
bool bm_owner_init(const mqtt_account_t *account, size_t maximum, mico_thread_t *owner, bool(*accept)(MessageData *, messageHandler));
int bm_client_init(Client *c, Network *n, unsigned timeout);
int bm_client_deinit(Client *c);
int bm_connect(Client *c, MQTTPacket_connectData *data);
int bm_disconnect(Client *c);
int bm_subscribe(Client *c, const char *filter, enum QoS qos, messageHandler handler);
int bm_yield(Client *c, int timeout);
int bm_resource_ready(void);
void bm_resource_round_end(bool complete);
int bm_publish(Client *c, const char *topic, MQTTMessage *m);
int bm_receipt_send(const mqtt_target_t *target, const char *wire, void *owned, void(*release_wire)(void *));
void bm_turn_begin(unsigned limit);
void bm_turn_end(void);
uint32_t bm_send_generation(void);
int bm_enqueue_send_generation(const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation);
int bm_enqueue_send(const char *topic, const void *payload, size_t n, unsigned qos, bool retain);
bool bm_input_consumed(void);
bool bm_work_pending(void);
int bm_poll(Client *c);
