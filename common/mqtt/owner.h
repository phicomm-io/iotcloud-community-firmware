/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_OWNER_H
#define PH_MQTT_OWNER_H
#include "mico.h"
/* Protocol-independent owner contract used by optional feature producers. */
enum { PH_MQTT_MORE = 2 };
/* Stable bound handle, available only to the current owner. */
mico_thread_t *ph_mqtt_owner_thread(void);
enum {
    PH_MQTT_ERROR_INIT = 1, PH_MQTT_ERROR_CONNECT, PH_MQTT_ERROR_SUBSCRIBE, PH_MQTT_ERROR_PUBLISH, PH_MQTT_ERROR_YIELD, PH_MQTT_ERROR_SESSION, PH_MQTT_ERROR_NETWORK,
    PH_MQTT_ERROR_APP_DISCOVERY_GRAPH, PH_MQTT_ERROR_APP_DISCOVERY_IDENTITY,
    PH_MQTT_ERROR_APP_DISCOVERY_ENCODING, PH_MQTT_ERROR_APP_OTA_GRAPH,
    PH_MQTT_ERROR_APP_OTA_ENCODING, PH_MQTT_ERROR_APP_SEND_CONVERSION,
    PH_MQTT_ERROR_APP_RECEIPT_ADMISSION
};
#endif
