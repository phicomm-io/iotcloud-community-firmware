/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mico.h"
#include "session.h"
#include "discovery_mp.h"
#include <string.h>
static void event(const ph_mqtt_session_hooks *h, unsigned state, int rc) {
    if (h->event)
        h->event(h->context, state, rc);
}

static bool session_valid(const ph_mqtt_session_hooks *h) {
    return (!h->stop || !h->stop(h->context)) && (!h->valid || h->valid(h->context));
}

static int publish_text(Client *client, const char *topic, const char *text) {
    MQTTMessage message = {0};
    message.qos = QOS1;
    message.retained = 1;
    message.payload = (void *) text;
    message.payloadlen = strlen(text);
    return ph_mqtt_publish(client, topic, &message);
}

static int open_session(Client *client, Network *network, const ph_mqtt_connection *s) {
    ph_mqtt_session_begin();
    memset(network, 0, sizeof(*network));
    network->my_socket = -1;
    ssl_opts options = {0};
    int rc = NewNetwork(network, (char *) s->host, s->port, options);
    if (rc != MQTT_SUCCESS) {
        ph_mqtt_error(PH_MQTT_ERROR_NETWORK, rc);
        return rc;
    }
    if (s->read)
        network->mqttread = s->read;
    rc = ph_mqtt_client_init(client, network, s->command_timeout_ms);
    if (rc != MQTT_SUCCESS)
        return rc;
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 4;
    data.clientID.cstring = (char *) s->client_id;
    data.username.cstring = s->user[0] ? (char *) s->user : NULL;
    data.password.cstring = s->user[0] ? (char *) s->password : NULL;
    data.keepAliveInterval = 30;
    data.cleansession = 1;
    data.willFlag = 1;
    data.will.topicName.cstring = (char *) s->availability;
    data.will.message.cstring = "offline";
    data.will.qos = 1;
    data.will.retained = 1;
    return ph_mqtt_connect(client, &data);
}

static void close_session(Client *client, Network *network) {
    /* Paho deinit does not close the Network. Close it even if init/connect
     * failed. This does not prove closed-kernel close resource reclamation. */
    if (client->isconnected)
        ph_mqtt_disconnect(client);
    if (network->disconnect)
        network->disconnect(network);
    ph_mqtt_client_deinit(client);
}

/* Bootstrap text has stable borrowed bytes. Keep it pending across preflight
 * refusal and service bounded protocol slices; never announce READY early. */
static __attribute__((noinline)) int publish_wait(Client *client, const char *topic, const void *bytes, size_t length,
                        const ph_mqtt_session_hooks *h) {
    for (;;) {
        if (!session_valid(h)) return PH_MQTT_RECONFIGURE;
        MQTTMessage message={0};message.qos=QOS1;message.retained=1;
        message.payload=(void*)bytes;message.payloadlen=length;
        int rc = ph_mqtt_publish(client,topic,&message);
        if (rc != PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED) return rc;
        rc = ph_mqtt_yield(client, 10);
        if (rc != MQTT_SUCCESS) return rc;
        mico_thread_msleep(10);
    }
}

static int announce(Client *client, const ph_mqtt_connection *spec, const ph_mqtt_session_hooks *h) {
    const mqtt_account_t *account=spec->account;
    if (!account->mode) return MQTT_SUCCESS;
    char topic[MQTT_TOPIC_MAX];unsigned char bytes[512];
    if (!mqtt_aux(account,"discovery",account->mac,topic,sizeof(topic))) return MQTT_FAILURE;
    size_t n=ph_mqtt_discovery_mp(account,spec->firmware_version,spec->variant,bytes,sizeof(bytes));
    if(!n)return MQTT_FAILURE;
    /* Fixed buffer lives across deferred retries; no unowned transient writer. */
    if(h)return publish_wait(client,topic,bytes,n,h);
    MQTTMessage message={0};message.qos=QOS1;message.retained=1;
    message.payload=bytes;message.payloadlen=n;return ph_mqtt_publish(client,topic,&message);
}

static __attribute__((noinline)) int subscribe_plan(Client *client, const ph_mqtt_session_hooks *h) {
    ph_mqtt_subscription subscription;
    for (unsigned i = 0; h->subscription(h->context, i, &subscription); i++) {
        if (i >= MAX_MESSAGE_HANDLERS)
            return MQTT_FAILURE;
        int rc = ph_mqtt_subscribe(client, subscription.filter, subscription.qos, subscription.handler);
        if (rc != MQTT_SUCCESS)
            return rc;
    }
    return MQTT_SUCCESS;
}

void ph_mqtt_session_run(Client *client, const ph_mqtt_session_hooks *h) {
    if (!client || !h || !h->prepare || !h->subscription || !h->initial || !h->cycle || !h->closing || !ph_mqtt_owner())
        return;
    unsigned attempt = 0;
    while (!h->stop || !h->stop(h->context)) {
        ph_mqtt_connection spec = {0};
        int ready = h->prepare(h->context, &spec);
        if (ready == PH_MQTT_PREPARE_STOP)
            break;
        if (ready != PH_MQTT_PREPARE_READY) {
            mico_thread_msleep(1000);
            continue;
        }
        Network network = {0};
        network.my_socket = -1;
        memset(client, 0, sizeof(*client));
        event(h, PH_MQTT_CONNECTING, 0);
        int rc = open_session(client, &network, &spec);
        bool connected = rc == MQTT_SUCCESS;
        if (!connected)
            event(h, PH_MQTT_CONNECT_ERROR, rc);
        if (rc == MQTT_SUCCESS)
            rc = announce(client, &spec, h);
        if (rc == MQTT_SUCCESS) {
            rc = subscribe_plan(client, h);
            if (rc != MQTT_SUCCESS)
                event(h, PH_MQTT_SUBSCRIBE_ERROR, rc);
        }
        if (rc == MQTT_SUCCESS) {
            ph_mqtt_turn_begin(4);
            rc = h->initial(h->context);
            ph_mqtt_turn_end();
            if (rc == PH_MQTT_MORE || rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
                rc = MQTT_SUCCESS;
        }
        /* The retained device availability is separate from sensor freshness.
         * Publish it once only after CONNECT, subscriptions and initial setup
         * succeed; a failed publication must prevent a false READY state. */
        bool online = false;
        if (rc == MQTT_SUCCESS && !session_valid(h))
            rc = PH_MQTT_RECONFIGURE;
        if (rc == MQTT_SUCCESS && (!h->initial_ready || h->initial_ready(h->context))) {
            rc = publish_text(client, spec.availability, "online");
            if (rc == MQTT_SUCCESS && !session_valid(h))
                rc = PH_MQTT_RECONFIGURE;
            if (rc == MQTT_SUCCESS) {
                online = true;
                attempt = 0;
                event(h, PH_MQTT_READY, 0);
            }
        }
        if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
            rc = MQTT_SUCCESS;
        while (rc == MQTT_SUCCESS && (!h->stop || !h->stop(h->context))) {
            rc = ph_mqtt_poll(client);
            if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
                rc = MQTT_SUCCESS;
            if (rc != MQTT_SUCCESS)
                break;
            /* Stop always wins. Identity changes caused by a command wait for
             * its captured receipt; no new native namespace work runs meanwhile. */
            if (h->stop && h->stop(h->context)) break;
            bool receipt_wait = ph_mqtt_receipt_pending();
            if (!session_valid(h) && !receipt_wait) {
                rc = PH_MQTT_RECONFIGURE;
                break;
            }
            rc = ph_mqtt_resource_ready();
            if (rc == MQTT_SUCCESS && (!receipt_wait || session_valid(h))) {
                ph_mqtt_turn_begin(4);
                rc = h->cycle(h->context);
                ph_mqtt_turn_end();
            }
            bool unfinished = rc == PH_MQTT_MORE || rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED || receipt_wait;
            if (rc == PH_MQTT_MORE || rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
                rc = MQTT_SUCCESS;
            if (rc != MQTT_SUCCESS)
                break;
            if (!session_valid(h) && !ph_mqtt_receipt_pending()) {
                rc = PH_MQTT_RECONFIGURE;
                break;
            }
            if (!online && session_valid(h) && (!h->initial_ready || h->initial_ready(h->context))) {
                rc = publish_text(client, spec.availability, "online");
                if (rc == MQTT_SUCCESS && !session_valid(h))
                    rc = PH_MQTT_RECONFIGURE;
                if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED) {
                    unfinished = true;
                    rc = MQTT_SUCCESS;
                } else if (rc != MQTT_SUCCESS)
                    break;
                else {
                    online = true;
                    attempt = 0;
                    event(h, PH_MQTT_READY, 0);
                }
            }
            /* Successful MQTT subscriptions persist for this connection.
             * Re-subscribing periodically replays retained configuration and
             * HA birth, creating avoidable RX allocations during publication.
             * A denied initial subscription already retries a fresh session;
             * namespace/filter edits are handled by session_valid(). */
            /* Keep both poll budgets and the product turn on every round.
             * A short positive protocol slice still reads/maintains keepalive;
             * idle owners retain their original pacing. No drain-until-empty. */
            if (rc == MQTT_SUCCESS) {
                bool active = unfinished || (h->initial_ready && !h->initial_ready(h->context));
                rc = ph_mqtt_yield(client, active || ph_mqtt_work_pending() ? 10 : 100);
            }
            if (rc == MQTT_SUCCESS)
                rc = ph_mqtt_poll(client);
            if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
                rc = MQTT_SUCCESS;
            /* The second poll can also complete a rename receipt. Do not
             * enter the next round's command poll before checking identity. */
            if (rc == MQTT_SUCCESS && (!h->stop || !h->stop(h->context)) &&
                !session_valid(h) && !ph_mqtt_receipt_pending())
                rc = PH_MQTT_RECONFIGURE;
            ph_mqtt_resource_round_end(rc == MQTT_SUCCESS);
            bool active = rc == MQTT_SUCCESS &&
                (unfinished || (h->initial_ready && !h->initial_ready(h->context)));
            mico_thread_msleep(rc == MQTT_SUCCESS && (active || ph_mqtt_work_pending()) ? 10 : 50);
        }
        /* Result send was completed by poll before a rename is noticed here.
         * Clear callbacks queued for this old session before model cleanup. */
        if (rc == PH_MQTT_RECONFIGURE)
            ph_mqtt_lifecycle_event(PH_MQTT_ERROR_SESSION, rc);
        else if (rc != MQTT_SUCCESS)
            ph_mqtt_error(PH_MQTT_ERROR_SESSION, rc);
        ph_mqtt_pending_clear();
        h->closing(h->context, connected, rc);
        if (connected)
            publish_text(client, spec.availability, "offline");
        close_session(client, &network);
        event(h, PH_MQTT_STOPPED, rc);
        if (!h->stop || !h->stop(h->context)) {
            ph_mqtt_reconnect();
            mico_thread_msleep(mqtt_retry_seconds(attempt < 5 ? attempt++ : 5) *1000);
        }
    }
}

int ph_mqtt_cleanup(Client *client, const mqtt_account_t *account, const char *host, unsigned port, const char *user, const char *password, int(*cleanup)(void)) {
    if (!ph_mqtt_owner())
        return MQTT_FAILURE;
    char id[192], topic[MQTT_TOPIC_MAX];
    if (!mqtt_identity(account, "", id, sizeof(id)) || !mqtt_leaf(account, "availability", topic, sizeof(topic)))
        return MQTT_FAILURE;
    ph_mqtt_connection spec = {account, host, user, password, id, topic, port, 5000, NULL, NULL, NULL};
    Network network = {0};
    network.my_socket = -1;
    memset(client, 0, sizeof(*client));
    int rc = open_session(client, &network, &spec);
    if (rc == MQTT_SUCCESS) {
        /* Withdraw only our old retained discovery, never advertise stale identity. */
        char discovery[MQTT_TOPIC_MAX];
        rc = MQTT_SUCCESS;
        if(account->mode)
            rc=mqtt_aux(account,"discovery",account->mac,discovery,sizeof(discovery)) ?
                publish_text(client,discovery,"") : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS)
            rc = cleanup();
        if (publish_text(client, topic, "offline") != MQTT_SUCCESS)
            rc = MQTT_FAILURE;
    }
    close_session(client, &network);
    return rc;
}
