/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Ports with a frozen typed-buffer ABI provide the same typed/auth interface
 * before this header. The parser and replay state machine remain shared. */
#ifndef PH_LAN_V4_TYPES_PROVIDED
#include "typed_wire.h"
#include "local_auth.h"
#endif
#ifndef PH_LAN_V4_BUFFER
#define PH_LAN_V4_BUFFER ph_buf
#endif
#ifndef PH_LAN_V4_BUFFER_LENGTH
#define PH_LAN_V4_BUFFER_LENGTH(b) ((b)->len)
#endif
#ifndef PH_LAN_V4_BUFFER_FREE
#define PH_LAN_V4_BUFFER_FREE(b) ph_buf_free(b)
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define LAN_V4_PORT 11288u
#define LAN_V4_MDNS_SERVICE "_openiot"
#define LAN_V4_MDNS_PROTO "_udp"
#define LAN_V4_MDNS_NAME "_openiot._udp.local."
#define LAN_V4_FRAME_MAX 1200u
#define LAN_V4_REQUEST_MAX 1024u
#define LAN_V4_RESPONSE_MAX 8192u
#define LAN_V4_CHUNK 768u
#define LAN_V4_RESPONSE_MS 5000u
#define LAN_V4_SESSIONS 4u
typedef enum { LAN_V4_COMMAND=1, LAN_V4_RESULT, LAN_V4_PULL, LAN_V4_ACK } lan_v4_operation;
typedef struct {
    lan_v4_operation op;
    char src[13],dst[13];
    uint8_t token[16];
    uint32_t boot,id,request_boot;
    uint16_t part,total,size;
    char code[20];
    /* Synchronous borrowed bin contents. Never retain beyond input lifetime. */
    const void *body;
    size_t body_length;
} lan_v4_packet;
/* Strict v4 schema. Decode validates the entire outer MP document; body is a
 * bin containing a complete TMCP MP request/result, validated by its consumer. */
bool lan_v4_decode_value(const ph_value *,lan_v4_packet *);
bool lan_v4_decode(const void *,size_t,lan_v4_packet *);
bool lan_v4_encode(ph_writer *,const lan_v4_packet *);

typedef struct {
    ph_auth_context context;
    uint8_t token[16],digest[32];
    uint32_t boot,last_id;
    bool used;
} lan_v4_session;
typedef struct {
    lan_v4_session sessions[LAN_V4_SESSIONS];
    PH_LAN_V4_BUFFER *response; /* Exactly one independently owned response, no copy. */
    uint32_t response_at,response_id;
    unsigned response_slot;
} lan_v4_server;
typedef struct {
    const char *local;
    uint32_t boot;
    uint32_t (*now)(void *);
    bool (*authorize)(void *,const char *,const uint8_t[16],ph_auth_context *);
    bool (*valid)(void *,const ph_auth_context *);
    bool (*digest)(void *,const void *,size_t,uint8_t[32]);
    /* Set admitted=false only when no service call/side effect took place.
     * An admitted NULL is indeterminate: its id is burned, never re-executed. */
    PH_LAN_V4_BUFFER *(*dispatch)(void *,const void *,size_t,const ph_auth_context *,bool *admitted);
    bool (*send)(void *,const char *,uint16_t,const void *,size_t);
    void *user;
} lan_v4_ops;
/* One serialized owner only. Input and scratch may alias: all borrowing ends
 * before reply encoding. No callback may re-enter this server. */
bool lan_v4_server_input(lan_v4_server *,const lan_v4_ops *,const lan_v4_packet *,
                         const char *ip,uint16_t port,void *scratch,size_t capacity);
void lan_v4_server_expire(lan_v4_server *,const lan_v4_ops *);
/* Pause drops bytes but preserves session high-water marks across OTA retries. */
void lan_v4_server_release(lan_v4_server *);
#ifdef __cplusplus
}
#endif
