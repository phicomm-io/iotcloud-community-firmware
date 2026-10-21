#ifndef TC1_SIGNED_ADAPTER_H
#define TC1_SIGNED_ADAPTER_H
#include "mico.h"
typedef struct {bool signature_verified,image_verified;uint32_t key_id,image_length;uint64_t release_counter;char version[32];} tc1_ota_metadata_t;
void tc1_ota_metadata(tc1_ota_metadata_t *);
void tc1_ota_init(void);
void tc1_ota_snapshot(char *,char *,unsigned *);
bool tc1_ota_reserve(void);
bool tc1_config_reserve(void);
void tc1_config_release(void);
void tc1_ota_abort(void);
void tc1_ota_failure(const char *reason);
bool tc1_ota_begin(uint32_t total, bool unsigned_rescue);
bool tc1_ota_write(const uint8_t *, size_t);
/* Verification changes no boot metadata; activation must follow an explicit
 * successful delivery/upgrade decision. finish combines both for downloads. */
bool tc1_ota_verify(void);
bool tc1_ota_activate(void);
bool tc1_ota_commit_uncertain(void);
bool tc1_ota_finish(void);
#endif
