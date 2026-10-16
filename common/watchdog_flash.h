#pragma once
#include "watchdog.h"
#include "mico.h"
#ifdef PHICOMM_DEVICE_OTA
#include "ota/mode.h"
#endif

/* MX1290 external SPI Flash erases 4 KiB sectors. Never issue a whole-chip
 * erase that could prevent the monitor from running for the entire OTA area. */
static bool watchdog_erase_ota(uint32_t length)
{
    if (!length || length % 4096u) return false;
    for (uint32_t offset = 0; offset < length; offset += 4096u) {
#ifdef PHICOMM_DEVICE_OTA
        if(ota_job_cancelled())return false;
#endif
        if (MicoFlashErase(MICO_PARTITION_OTA_TEMP, offset, 4096u) != kNoErr ||
            !watchdog_ota_progress()) return false;
#ifdef PHICOMM_DEVICE_OTA
        mico_thread_msleep(1);
#endif
    }
    return true;
}

static bool watchdog_read_ota(uint32_t offset, void *data, uint32_t length)
{
    return MicoFlashRead(MICO_PARTITION_OTA_TEMP, &offset, data, length) == kNoErr &&
        watchdog_ota_progress();
}
