#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef PHICOMM_DEVICE_OTA
#include "../typed_wire.h"
void ota_job_init(void);
bool ota_job_claim(void);
bool ota_job_enter(void);
void ota_job_release(void);
/* Non-waiting recovery attempt, throttled internally; call outside business locks. */
void ota_job_poll(void);
bool ota_job_cancel(void);
bool ota_job_cancelled(void);
bool ota_job_commit(void);
void ota_job_sample(void);
bool ota_job_active(void);
void ota_job_status_mp(ph_writer *output);
#else
static inline void ota_job_init(void){}
static inline bool ota_job_active(void){return false;}
static inline void ota_job_poll(void){}
#endif
