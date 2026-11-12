/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../protocol/typed_wire.h"
/* Reuse the public LAN implementation with the frozen printbuf writer ABI. */
#define PH_LAN_V4_TYPES_PROVIDED
#define PH_LAN_V4_BUFFER struct printbuf
#define PH_LAN_V4_BUFFER_LENGTH(b) ((size_t)(b)->bpos)
#define PH_LAN_V4_BUFFER_FREE(b) printbuf_free(b)
#include "../common/lan_wire_v4.h"

typedef struct {
    const void *bytes;
    size_t length;
    ph_value params; /* Borrowed only until dispatch returns. */
    char action[48], id[65], receipt_key[65];
} ph_lan_v4_request;
typedef struct {
    void *user;
    uint32_t (*now)(void *);
    size_t (*free_heap)(void *);
    bool (*digest)(void *,const void *,size_t,uint8_t[32]);
    /* The response capacity is independently owned and reserved before this
     * call. Write exactly once; false admission means no service/side effect.
     * All request views expire on return. Never elevate the context to admin. */
    size_t (*dispatch)(void *,const ph_lan_v4_request *,const ph_auth_context *,
                       void *,size_t,bool *admitted);
} ph_lan_v4_ops;
typedef struct {
    lan_v4_server server;
    ph_lan_v4_ops ops;
    ph_auth_state *auth;
    char local[13];
    uint32_t boot;
    /* MQTT SENSOR credentials carry a broker-selected source boot. Never
     * accept a first datagram's boot as a replacement for that authority. */
    uint32_t mqtt_boot[PH_AUTH_MQTT_SLOTS],input_boot;
    /* Synchronous owner workspace, cleared before feed returns. */
    void *frame;
    size_t frame_capacity,frame_length;
    bool feeding;
} ph_lan_v4;
/* One serialized network owner; caller holds the product/auth mutex. */
bool ph_lan_v4_init(ph_lan_v4 *,const char *local,uint32_t boot,
                    ph_auth_state *,const ph_lan_v4_ops *);
/* Only an already broker/ACL/target-validated, non-retained sensor command
 * invokes this under the product/auth lock. Grants SENSOR alone. */
bool ph_lan_v4_sensor_grant(ph_lan_v4 *,const char *peer,const char *token_text,uint32_t boot);
/* Replies always target the exact supplied source IP/port. Send the returned
 * bytes after releasing the product mutex. Input/frame may alias. */
size_t ph_lan_v4_feed(ph_lan_v4 *,const void *,size_t,const char *,uint16_t,
                     void *frame,size_t capacity);
void ph_lan_v4_poll(ph_lan_v4 *);
/* OTA/bind pause: drop retained bytes, retain replay high-water marks. */
void ph_lan_v4_pause(ph_lan_v4 *);
/* Credential reset: release bytes before resetting the session table. */
void ph_lan_v4_revoke(ph_lan_v4 *);
