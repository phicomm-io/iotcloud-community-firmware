/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OBK_CONFIG_H
#define OBK_CONFIG_H
#define OBK_VARIANT_DEFAULT 0
#define OBK_VARIANT_BERRY 1
#define OBK_VARIANT_TUYAMCU 2
#define OBK_VARIANT_POWERMETERING 3
#define OBK_VARIANT_IRREMOTEESP 4
#define OBK_VARIANT_SENSORS 5
#define OBK_VARIANT_HLW8112 6
#define OBK_VARIANT_BATTERY 7
#define OBK_VARIANT_BTPROXY 8
#define OBK_VARIANT_XR806_DCDC 9
#define OBK_VARIANT_XIAOMI_COMPACT4 10
/* Dedicated socket firmware: OpenBeken HAL, RTOS, Wi-Fi, configuration, HTTP,
 * littlefs and OTA; project protocols exclusively own networking and GPIO. */
#define OBK_DISABLE_ALL_DRIVERS 1
#define ENABLE_LITTLEFS 1
#define ENABLE_HTTP_MQTT 1
#define ENABLE_HTTP_IP 1
#define ENABLE_HTTP_NAMES 1
#define ENABLE_SEND_POSTANDGET 1
#define ENABLE_MQTT 0
#define ENABLE_HA_DISCOVERY 0
#define ENABLE_TASMOTA_JSON 0
#define ENABLE_OBK_SCRIPTING 0
#define ENABLE_OBK_BERRY 0
#define ENABLE_LED_BASIC 0
#define NEW_TCP_SERVER 0
#endif
