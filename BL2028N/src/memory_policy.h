/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Dedicated service stacks are already reserved at boot. Retain 8 KiB during
 * ordinary MQTT allocations; OTA reclaims the optional 2 KiB status cache
 * before admitting its downloader. Keep the downloader's 12 KiB threshold. */
#define PH_SERVICE_MEMORY_RESERVE 8192u
#define PH_OTA_START_HEAP 12288u
/* Manual AP scans initialize station state and retain beacon IE buffers. */
#define PH_SCAN_START_HEAP (PH_SERVICE_MEMORY_RESERVE + 4096u)
#define PH_SCAN_RUNNING_HEAP (PH_SERVICE_MEMORY_RESERVE + 1024u)
