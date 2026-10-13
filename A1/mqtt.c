#include "a1.h"
#include "MQTTClient.h"
#include <ctype.h>

static Client client;
static char base[64], availability_topic[96];
static bool rediscover;
static int publish(const char *topic, const char *payload, bool retain)
{
    MQTTMessage m = {0}; m.qos = QOS0; m.retained = retain;
    m.payload = (void *)payload; m.payloadlen = strlen(payload);
    return MQTTPublish(&client, topic, &m);
}
static int leaf(const char *name, const char *payload)
{
    char topic[96]; snprintf(topic, sizeof(topic), "%s/%s", base, name);
    return publish(topic, payload, true);
}
static bool topic_is(MessageData *d, const char *suffix)
{
    char topic[96]; snprintf(topic, sizeof(topic), "%s/%s", base, suffix);
    MQTTString *t = d->topicName;
    if (t->cstring) return !strcmp(t->cstring, topic);
    return t->lenstring.len == (int)strlen(topic) && !memcmp(t->lenstring.data, topic, t->lenstring.len);
}
static void command(MessageData *d)
{
    MQTTMessage *m = d->message;
    if (m->retained || !m->payloadlen || m->payloadlen > A1_JSON_LIMIT) return;
    /* Wildcard also receives our own states: dispatch only these exact commands. */
    const char *kind = NULL, *key = NULL;
    if (topic_is(d, "fan/set")) { kind = "fan"; key = "on"; }
    else if (topic_is(d, "fan/percentage/set")) { kind = "fan"; key = "speed"; }
    else if (topic_is(d, "light/set")) kind = "light";
    else if (topic_is(d, "child_lock/set")) { kind = "child_lock"; key = "enabled"; }
    else if (topic_is(d, "led_always_on/set")) { kind = "light"; key = "always_on"; }
    else return;
    char *text = malloc(m->payloadlen + 1);
    if (!text) return;
    memcpy(text, m->payload, m->payloadlen); text[m->payloadlen] = 0;
    if (memchr(text, 0, m->payloadlen)) { free(text); return; }
    json_object *o = NULL;
    if (!key) o = a1_json_parse(text, m->payloadlen);
    else {
        o = json_object_new_object();
        if (o && !strcmp(key, "speed")) {
            char *end; long n = strtol(text, &end, 10);
            while (isspace((unsigned char)*end)) end++;
            if (end == text || *end || n < 0 || n > 100 || !isdigit((unsigned char)text[0])) {
                json_object_put(o); o = NULL;
            } else json_object_object_add(o, key, json_object_new_int(n));
        } else if (o && (!strcmp(text, "ON") || !strcmp(text, "OFF")))
            json_object_object_add(o, key, json_object_new_boolean(!strcmp(text, "ON")));
        else if (o) { json_object_put(o); o = NULL; }
    }
    if (o) { a1_control_json(kind, o); json_object_put(o); }
    free(text);
}
static void ha_birth(MessageData *d)
{
    if (d->message->payloadlen == 6 && !memcmp(d->message->payload, "online", 6)) rediscover = true;
}
static void str(json_object *o, const char *key, const char *value)
{ json_object_object_add(o, key, json_object_new_string(value)); }
static void topic(json_object *o, const char *key, const char *suffix)
{
    char value[96]; snprintf(value, sizeof(value), "%s/%s", base, suffix); str(o, key, value);
}
static int discovery(const a1_config_t *c)
{
    const char *keys[] = {"fan", "light", "child_lock", "led_always_on"};
    const char *components[] = {"fan", "light", "switch", "switch"};
    const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮"};
    for (unsigned i = 0; i < 4; i++) {
        json_object *o = json_object_new_object(), *device = json_object_new_object(), *ids = json_object_new_array();
        if (!o || !device || !ids) {
            if (o) json_object_put(o);
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            return MQTT_FAILURE;
        }
        char text[128], path[128];
        snprintf(text, sizeof(text), "a1_%s", a1_id);
        json_object_array_add(ids, json_object_new_string(text));
        json_object_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_object_object_add(o, "device", device); str(o, "name", names[i]);
        snprintf(text, sizeof(text), "a1_%s_%s", a1_id, keys[i]); str(o, "unique_id", text);
        str(o, "availability_topic", availability_topic);
        json_object_object_add(o, "retain", json_object_new_boolean(false));
        snprintf(text, sizeof(text), "%s/set", keys[i]); topic(o, "command_topic", text);
        snprintf(text, sizeof(text), "%s/state", keys[i]); topic(o, "state_topic", text);
        if (i == 0) {
            str(o, "icon", "mdi:air-purifier");
            topic(o, "percentage_command_topic", "fan/percentage/set");
            topic(o, "percentage_state_topic", "fan/percentage/state");
            json_object_object_add(o, "speed_range_min", json_object_new_int(1));
            json_object_object_add(o, "speed_range_max", json_object_new_int(100));
        }
        if (i == 1) {
            str(o, "schema", "json");
            json_object_object_add(o, "brightness", json_object_new_boolean(true));
            json_object *modes = json_object_new_array();
            if (!modes) { json_object_put(o); return MQTT_FAILURE; }
            json_object_array_add(modes, json_object_new_string("rgb"));
            json_object_object_add(o, "supported_color_modes", modes);
        } else { str(o, "payload_on", "ON"); str(o, "payload_off", "OFF"); }
        if (i >= 2) str(o, "entity_category", "config");
        snprintf(path, sizeof(path), "homeassistant/%s/a1_%s/%s/config", components[i], a1_id, keys[i]);
        int rc = publish(path, json_object_to_json_string(o), true); json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}
static int states(const a1_config_t *c, const a1_state_t *s)
{
    int rc = leaf("fan/state", c->fan_on ? "ON" : "OFF");
    char speed[4], light[192]; snprintf(speed, sizeof(speed), "%u", c->fan_on ? c->speed : 0);
    if (rc == MQTT_SUCCESS) rc = leaf("fan/percentage/state", speed);
    snprintf(light, sizeof(light), "{\"state\":\"%s\",\"color_mode\":\"rgb\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
        s->led_visible ? "ON" : "OFF", c->brightness, c->red, c->green, c->blue);
    if (rc == MQTT_SUCCESS) rc = leaf("light/state", light);
    if (rc == MQTT_SUCCESS) rc = leaf("child_lock/state", c->child_lock ? "ON" : "OFF");
    if (rc == MQTT_SUCCESS) rc = leaf("led_always_on/state", c->led_always_on ? "ON" : "OFF");
    return rc;
}
static bool same_connection(const a1_config_t *a, const a1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) && !strcmp(a->name, b->name);
}
static void mqtt_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    char client_id[24], wildcard[96]; snprintf(client_id, sizeof(client_id), "phicomm_a1_%s", a1_id);
    snprintf(base, sizeof(base), "phicomm/a1/%s", a1_id);
    snprintf(wildcard, sizeof(wildcard), "%s/#", base);
    snprintf(availability_topic, sizeof(availability_topic), "%s/availability", base);
    for (;;) {
        a1_config_t cfg, current; a1_state_t state;
        a1_config_get(&cfg); a1_state_get(&state);
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mico_thread_msleep(1000); continue; }
        Network net = {0}; net.my_socket = -1; memset(&client, 0, sizeof(client));
        ssl_opts options = {0};
        if (NewNetwork(&net, cfg.mqtt_host, cfg.mqtt_port, options) != MQTT_SUCCESS) { mico_thread_msleep(3000); continue; }
        if (MQTTClientInit(&client, &net, 5000) != MQTT_SUCCESS) { net.disconnect(&net); mico_thread_msleep(3000); continue; }
        MQTTPacket_connectData connect = MQTTPacket_connectData_initializer;
        connect.MQTTVersion = 4; connect.clientID.cstring = client_id;
        connect.username.cstring = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL;
        connect.password.cstring = cfg.mqtt_user[0] ? cfg.mqtt_password : NULL;
        connect.keepAliveInterval = 30; connect.cleansession = 1;
        connect.willFlag = 1; connect.will.topicName.cstring = availability_topic;
        connect.will.message.cstring = "offline"; connect.will.retained = 1; connect.will.qos = 1;
        bool connected = MQTTConnect(&client, &connect) == MQTT_SUCCESS;
        int rc = connected ? MQTTSubscribe(&client, wildcard, QOS0, command) : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS) rc = MQTTSubscribe(&client, "homeassistant/status", QOS0, ha_birth);
        if (rc == MQTT_SUCCESS) rc = discovery(&cfg);
        if (rc == MQTT_SUCCESS) rc = publish(availability_topic, "online", true);
        bool sent = false, old_visible = false; uint32_t revision = 0; rediscover = false;
        while (rc == MQTT_SUCCESS) {
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = true; mico_rtos_unlock_mutex(&a1_lock);
            a1_state_get(&state); a1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) break;
            if (rediscover) { rc = discovery(&current); rediscover = false; sent = false; }
            if (rc == MQTT_SUCCESS && (!sent || revision != current.revision || old_visible != state.led_visible)) {
                rc = states(&current, &state); revision = current.revision; old_visible = state.led_visible; sent = true;
            }
            if (rc == MQTT_SUCCESS) rc = MQTTYield(&client, 100);
            mico_thread_msleep(50);
        }
        mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&a1_lock);
        if (connected) { publish(availability_topic, "offline", true); MQTTDisconnect(&client); }
        net.disconnect(&net); MQTTClientDeinit(&client); mico_thread_msleep(3000);
    }
}
OSStatus a1_mqtt_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_mqtt", mqtt_worker, 8192, 0); }
