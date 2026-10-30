/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Dedicated service stacks are already reserved at boot. The measured idle
 * heap is 13 KiB; retain 8 KiB during ordinary MQTT allocations. */
#define PH_SERVICE_MEMORY_RESERVE 8192u
#define PH_OTA_START_HEAP 12288u
