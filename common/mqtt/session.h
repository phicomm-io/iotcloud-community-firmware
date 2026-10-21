/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_SESSION_H
#define PH_MQTT_SESSION_H
#include "client.h"
/* All strings are adapter-owned and immutable until closing() returns.
 * Subscription filters remain alive until the shared owner deinitializes Paho. */
typedef struct {
    const mqtt_account_t *account;
    const char *host, *user, *password, *client_id, *availability;
    unsigned port, command_timeout_ms;
    int(*read)(Network *, unsigned char *, int, int);
}

ph_mqtt_connection;
typedef struct {
    const char *filter;
    enum QoS qos;
    messageHandler handler;
}

ph_mqtt_subscription;
enum {
    PH_MQTT_RECONFIGURE = 1, PH_MQTT_PREPARE_WAIT = 0, PH_MQTT_PREPARE_READY = 1, PH_MQTT_PREPARE_STOP = -1
};
enum {
    PH_MQTT_CONNECTING, PH_MQTT_CONNECT_ERROR, PH_MQTT_SUBSCRIBE_ERROR, PH_MQTT_READY, PH_MQTT_STOPPED
};
typedef struct {
    void *context;
    int(*prepare)(void *, ph_mqtt_connection *);
    bool(*subscription)(void *, unsigned, ph_mqtt_subscription *);
    int(*initial)(void *);
    int(*cycle)(void *);
    void(*closing)(void *, bool, int);
    void(*event)(void *, unsigned, int);
    bool(*stop)(void *);
    /* Optional owner-only predicate: batched discovery/initial state is done. */
    bool(*initial_ready)(void *);
    /* Owner-only fresh identity/network check, with no lock held over I/O. */
    bool(*valid)(void *);
}

ph_mqtt_session_hooks;
/* One owner runs this lifecycle. Paho's protocol deadlines are retained; a
 * closed SDK DNS/socket/close call is not an application-bounded operation. */
void ph_mqtt_session_run(Client *, const ph_mqtt_session_hooks *);
/* Best effort previous-session cleanup uses the same open/close machinery. */
int ph_mqtt_cleanup(Client *, const mqtt_account_t *, const char *, unsigned, const char *, const char *, int(*)(void));
#endif
