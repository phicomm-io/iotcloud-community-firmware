/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Dedicated service stacks are already reserved at boot. Retain 8 KiB during
 * ordinary MQTT allocations; OTA reclaims the optional 2 KiB status cache
 * before admitting its downloader. Keep the downloader's 12 KiB threshold. */
#define PH_SERVICE_MEMORY_RESERVE 8192u
#define PH_OTA_START_HEAP 12288u
