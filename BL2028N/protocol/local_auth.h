#ifndef PH_LOCAL_AUTH_H
#define PH_LOCAL_AUTH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PH_AUTH_TOKEN_BYTES 16
#define PH_AUTH_TOKEN_TEXT 33
#define PH_AUTH_HTTP_SLOTS 4
#define PH_AUTH_PAIR_SLOTS 12
#define PH_AUTH_MQTT_SLOTS 4
#define PH_AUTH_IDLE_MS UINT32_C(1800000)
#define PH_AUTH_MAX_MS UINT32_C(86400000)
#define PH_AUTH_STORE_MAGIC UINT32_C(0x31415550)
#define PH_AUTH_STORE_VERSION 1
#define PH_AUTH_SCOPE_CONTROL 1u
#define PH_AUTH_SCOPE_SENSOR 2u
#define PH_AUTH_SCOPE_GROUP 4u
#define PH_AUTH_SCOPE_ADMIN 8u
#define PH_AUTH_SCOPE_ALL 15u
typedef enum { PH_AUTH_NONE=0, PH_AUTH_HTTP=1, PH_AUTH_UDP_PAIR=2,
 PH_AUTH_UDP_MQTT=3, PH_AUTH_MQTT=4, PH_AUTH_INTERNAL=5 } ph_auth_source;
typedef struct { char peer[13]; uint8_t key[16]; uint8_t scopes; uint8_t reserved[2]; } ph_auth_pair_record;
/* Append-only trailer, never insert into the frozen product configuration prefix. */
typedef struct { uint32_t magic; uint16_t version; uint16_t count;
 ph_auth_pair_record pairs[PH_AUTH_PAIR_SLOTS]; } ph_auth_store;
typedef struct { uint8_t token[16]; uint32_t born,last; uint32_t generation; uint8_t active,scopes; char peer[13]; } ph_auth_session;
typedef struct ph_auth_state ph_auth_state;
typedef struct { ph_auth_state *owner; ph_auth_source source; uint32_t generation;
 uint8_t slot,scopes; char peer[13]; } ph_auth_context;
typedef struct {
 void *user;
 uint32_t (*now)(void *);
 bool (*random)(void *,uint8_t *,size_t);
 void (*lock)(void *); void (*unlock)(void *);
 /* Copy current password (maximum 32 bytes + NUL), under the same lock. */
 bool (*password)(void *,char out[33]);
 /* Persist entire trailer atomically. False means storage remains unchanged. */
 bool (*save)(void *,const ph_auth_store *);
} ph_auth_ops;
struct ph_auth_state { ph_auth_ops ops; ph_auth_store store;
 ph_auth_session http[PH_AUTH_HTTP_SLOTS],local[PH_AUTH_PAIR_SLOTS],mqtt[PH_AUTH_MQTT_SLOTS];
 uint32_t generation,credential_epoch; char credential[33]; };
void ph_auth_store_empty(ph_auth_store *);
bool ph_auth_store_valid(const ph_auth_store *);
bool ph_auth_init(ph_auth_state *,const ph_auth_ops *,const ph_auth_store *);
bool ph_auth_http_login(ph_auth_state *,const char *password,char token[33]);
bool ph_auth_http_check(ph_auth_state *,const char *authorization,ph_auth_context *);
bool ph_auth_http_logout(ph_auth_state *,const char *authorization);
/* Pair returns a durable secret. Resume exchanges it for a boot-local token. */
/* Correct password + existing peer/scopes returns its durable key without a write.
 * Revocation or a scope change starts a new pairing credential. */
bool ph_auth_pair(ph_auth_state *,const char *password,const char *peer,unsigned scopes,char key[33]);
/* Matching current key reuses this boot's live local token, without extending
 * its idle/max lifetime. Expired/revoked/rebooted sessions cannot be reused. */
bool ph_auth_pair_resume(ph_auth_state *,const char *peer,const char *key,char token[33]);
bool ph_auth_pair_remove(ph_auth_state *,const char *peer);
bool ph_auth_udp_check(ph_auth_state *,const char *peer,const char *token,ph_auth_context *);
/* Only the already broker/ACL/target-validated MQTT adapter may call these. */
bool ph_auth_mqtt_grant(ph_auth_state *,const char *peer,const char *token,unsigned scopes);
void ph_auth_mqtt_context(ph_auth_state *,ph_auth_context *);
void ph_auth_internal_context(ph_auth_state *,ph_auth_context *);
bool ph_auth_pair_store(ph_auth_state *,const char *peer,const char *key,unsigned scopes);
bool ph_auth_pair_key(ph_auth_state *,const char *peer,char key[33]);
bool ph_auth_pair_peer(ph_auth_state *,unsigned index,char peer[13],unsigned *scopes);
bool ph_auth_pair_store_epoch(ph_auth_state *,const char *peer,const char *key,unsigned scopes,uint32_t expected_epoch);
/* Called after a password change has persisted. Clears local pairs durably first. */
bool ph_auth_password_changed(ph_auth_state *);
void ph_auth_local_revoke(ph_auth_state *,const ph_auth_store *);
ph_auth_state *device_local_auth(void);
bool device_local_auth_init(void);
void device_local_auth_revoke(void);
bool ph_auth_context_valid(const ph_auth_context *,unsigned required_scopes);
uint32_t ph_auth_credential_epoch(ph_auth_state *);
bool ph_auth_token_decode(const char *,uint8_t out[16]);
#ifdef __cplusplus
}
#endif
#endif
