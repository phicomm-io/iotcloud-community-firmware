/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Retain 8 KiB during ordinary MQTT allocations. The MQTT owner stack is
 * reserved on demand and released for OTA; reclaim the optional 2 KiB status
 * cache before admitting the downloader at its original 12 KiB threshold. */
#define PH_SERVICE_MEMORY_RESERVE 8192u
#define PH_OTA_START_HEAP 12288u
/* Manual AP scans initialize station state and retain beacon IE buffers. */
#define PH_SCAN_START_HEAP (PH_SERVICE_MEMORY_RESERVE + 4096u)
#define PH_SCAN_RUNNING_HEAP (PH_SERVICE_MEMORY_RESERVE + 1024u)
/* Include owner startup buffers/RTOS overhead above the ordinary reserve. */
#define PH_MQTT_STACK_BYTES 16384u
#define PH_MQTT_START_HEAP (PH_SERVICE_MEMORY_RESERVE + PH_MQTT_STACK_BYTES + 2048u)
