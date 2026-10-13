#pragma once
#include "MQTTClient.h"
#include "mqtt_account.h"

/* Best-effort cleanup when settings changed during an offline retry. The
 * caller retains the old account and CONNECT credentials until this returns. */
static inline int mqtt_cleanup_reconnect(Client *client, const mqtt_account_t *account,
                                         const char *host, unsigned port,
                                         const char *user, const char *password,
                                         int (*cleanup)(void))
{
    Network net = {0}; net.my_socket = -1;
    ssl_opts options = {0};
    memset(client, 0, sizeof(*client));
    int result = NewNetwork(&net, (char *)host, port, options);
    if (result != MQTT_SUCCESS) return result;
    result = MQTTClientInit(client, &net, 5000);
    if (result != MQTT_SUCCESS) { net.disconnect(&net); return result; }
    char id[192], topic[MQTT_TOPIC_MAX];
    mqtt_identity(account, "", id, sizeof(id));
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 4; data.clientID.cstring = id;
    data.username.cstring = (char *)user; data.password.cstring = (char *)password;
    data.cleansession = 1; data.keepAliveInterval = 30;
    mqtt_leaf(account, "availability", topic, sizeof(topic));
    data.willFlag = 1; data.will.topicName.cstring = topic;
    data.will.message.cstring = "offline"; data.will.qos = 1; data.will.retained = 1;
    result = MQTTConnect(client, &data);
    if (result == MQTT_SUCCESS) {
        MQTTMessage message = {0}; message.qos = QOS1; message.retained = 1;
        if (account->mode) {
            char declaration[MQTT_TOPIC_MAX];
            mqtt_aux(account, "discovery", account->mac, declaration, sizeof(declaration));
            message.payload = (void *)mqtt_announcement(account);
            message.payloadlen = strlen(message.payload);
            result = MQTTPublish(client, declaration, &message);
        }
        if (result == MQTT_SUCCESS) result = cleanup();
        message.payload = "offline"; message.payloadlen = 7;
        if (MQTTPublish(client, topic, &message) != MQTT_SUCCESS) result = MQTT_FAILURE;
        MQTTDisconnect(client);
    }
    net.disconnect(&net); MQTTClientDeinit(client);
    return result;
}
