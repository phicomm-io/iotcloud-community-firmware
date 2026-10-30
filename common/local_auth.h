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
#define PH_AUTH_UNPAIR_RECEIPTS 4
#define PH_AUTH_UNPAIR_RECEIPT_MS UINT32_C(30000)
#define PH_AUTH_STORE_MAGIC UINT32_C(0x31415550)
#define PH_AUTH_STORE_VERSION 1
#define PH_AUTH_SCOPE_CONTROL 1u
#define PH_AUTH_SCOPE_SENSOR 2u
#define PH_AUTH_SCOPE_GROUP 4u
#define PH_AUTH_SCOPE_ADMIN 8u
#define PH_AUTH_SCOPE_ALL 15u
typedef enum { PH_AUTH_NONE=0, PH_AUTH_HTTP=1, PH_AUTH_UDP_PAIR=2,
 PH_AUTH_UDP_MQTT=3, PH_AUTH_MQTT=4, PH_AUTH_INTERNAL=5,
 PH_AUTH_HTTP_OPEN=6, PH_AUTH_HTTP_BASIC=7 } ph_auth_source;
typedef struct { char peer[13]; uint8_t key[16]; uint8_t scopes; uint8_t reserved[2]; } ph_auth_pair_record;
/* Append-only trailer, never insert into the frozen product configuration prefix. */
typedef struct { uint32_t magic; uint16_t version; uint16_t count;
 ph_auth_pair_record pairs[PH_AUTH_PAIR_SLOTS];
 /* Outbound secrets never authorize an incoming datagram. */
 uint16_t remote_count,reserved;ph_auth_pair_record remote[PH_AUTH_PAIR_SLOTS]; } ph_auth_store;
typedef struct { uint8_t token[16]; uint32_t born,last; uint32_t generation; uint8_t active,scopes; char peer[13]; } ph_auth_session;
/* Boot-local success receipts; never persisted or exported. IPv4 adapters use
 * canonical dotted addresses. Four entries bound RAM and retry admission. */
typedef struct { uint32_t born,epoch; uint8_t token[16],nonce[16];
 char peer[13],ip[16]; uint8_t active; } ph_auth_unpair_receipt;
typedef struct ph_auth_state ph_auth_state;
typedef struct { ph_auth_state *owner; ph_auth_source source; uint32_t generation,credential_epoch;
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
 ph_auth_unpair_receipt unpair[PH_AUTH_UNPAIR_RECEIPTS];
 /* Volatile business-client boot, indexed by local session slot. Never part
  * of ph_auth_store or a backup/configuration/Flash schema. */
 uint32_t local_boot[PH_AUTH_PAIR_SLOTS];
 uint32_t generation,credential_epoch; char credential[33]; };
void ph_auth_store_empty(ph_auth_store *);
/* Immutable empty trailer for synchronous commits; avoids large stack copies. */
const ph_auth_store *ph_auth_store_empty_value(void);
bool ph_auth_store_valid(const ph_auth_store *);
/* Recognize the frozen inbound-only v1 prefix and an entirely erased appended
 * directional area. Preserve every prefix byte; reject unknown/mixed trailers. */
bool ph_auth_store_migrate_legacy(ph_auth_store *);
bool ph_auth_init(ph_auth_state *,const ph_auth_ops *,const ph_auth_store *);
bool ph_auth_http_login(ph_auth_state *,const char *password,char token[33]);
typedef enum { PH_AUTH_LOGIN_OK,PH_AUTH_LOGIN_DENIED,PH_AUTH_LOGIN_BUSY } ph_auth_login_result;
/* An unset password returns OK with an empty token and consumes no session. */
ph_auth_login_result ph_auth_http_login_result(ph_auth_state *,const char *,char token[33]);
bool ph_auth_password_is_set(ph_auth_state *);
/* Capture once at HTTP admission, then revalidate this context before dispatch.
 * Open/Basic contexts use the credential epoch and never occupy token slots. */
bool ph_auth_http_check(ph_auth_state *,const char *authorization,ph_auth_context *);
bool ph_auth_http_logout(ph_auth_state *,const char *authorization);
/* Pair returns a durable secret. Resume exchanges it for a boot-local token. */
/* Correct password + existing peer/scopes returns its durable key without a write.
 * Revocation or a scope change starts a new pairing credential. */
bool ph_auth_pair(ph_auth_state *,const char *password,const char *peer,unsigned scopes,char key[33]);
/* Matching current key reuses this boot's live local token, without extending
 * its idle/max lifetime. Expired/revoked/rebooted sessions cannot be reused. */
bool ph_auth_pair_resume(ph_auth_state *,const char *peer,const char *key,char token[33]);
/* Business resume: a changed nonzero client boot rotates only this peer's
 * token; identical boot retries reuse it without extending idle/max lifetime. */
bool ph_auth_pair_resume_boot(ph_auth_state *,const char *,const char *,uint32_t boot,char token[33]);
bool ph_auth_pair_remove(ph_auth_state *,const char *peer);
bool ph_auth_pair_unpair(ph_auth_state *,const char *peer,const char *token);
/* auth_unpair only: an exact retry may reuse a successful durable revocation
 * for 30s. No receipt on persistence failure; epoch changes invalidate it. */
bool ph_auth_pair_unpair_request(ph_auth_state *,const char *peer,const char *token,
                                const char *ipv4,const uint8_t nonce[16]);
bool ph_auth_udp_check(ph_auth_state *,const char *peer,const char *token,ph_auth_context *);
/* Only the already broker/ACL/target-validated MQTT adapter may call these. */
bool ph_auth_mqtt_grant(ph_auth_state *,const char *peer,const char *token,unsigned scopes);
void ph_auth_mqtt_context(ph_auth_state *,ph_auth_context *);
void ph_auth_internal_context(ph_auth_state *,ph_auth_context *);
bool ph_auth_pair_store(ph_auth_state *,const char *peer,const char *key,unsigned scopes);
bool ph_auth_pair_key(ph_auth_state *,const char *peer,char key[33]);
bool ph_auth_pair_peer(ph_auth_state *,unsigned index,char peer[13],unsigned *scopes);
bool ph_auth_pair_store_epoch(ph_auth_state *,const char *peer,const char *key,unsigned scopes,uint32_t expected_epoch);
bool ph_auth_remote_get(ph_auth_state *,const char *peer,char key[33],unsigned *scopes);
bool ph_auth_remote_store(ph_auth_state *,const char *peer,const char *key,unsigned scopes,uint32_t expected_epoch);
bool ph_auth_remote_remove(ph_auth_state *,const char *peer);
/* Revoke phase BEFORE persisting a new password. true means the pairing
 * store is durably empty and the caller may then commit the replacement
 * password. false means DO NOT commit it: old durable pairs may return on
 * reboot, although this boot forgets them and clears HTTP/local sessions.
 * A crash between successful revoke and password commit only loses pairs. */
bool ph_auth_password_changed(ph_auth_state *);
/* Serialize configuration and pairing changes in one durable transaction.
 * commit runs under the auth lock and must atomically persist the supplied
 * trailer together with new_password. It may take the product config lock,
 * but must not re-enter auth. On false, core auth state is not published.
 * The adapter must keep config coherent; if the durable result is uncertain,
 * it must enter recovery and reject authorization, including queued contexts,
 * until storage is reloaded. False alone does not prove storage unchanged.
 * force_revoke is used by backup restore, even when its password is unchanged. */
typedef bool (*ph_auth_commit_fn)(void *,const ph_auth_store *);
bool ph_auth_credential_commit(ph_auth_state *,const char *new_password,bool force_revoke,
                              ph_auth_commit_fn,void *);
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
