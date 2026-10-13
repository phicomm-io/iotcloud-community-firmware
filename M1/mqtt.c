#include "m1.h"
#include "MQTTClient.h"

static Client client;
static char base[64], command_topic[96], availability_topic[96];
static bool rediscover;

static int publish(const char *topic, const char *payload, bool retain)
{
    MQTTMessage message = {0};
    message.qos = QOS0; message.retained = retain;
    message.payload = (void *)payload; message.payloadlen = strlen(payload);
    return MQTTPublish(&client, topic, &message);
}

static int publish_leaf(const char *leaf, const char *payload, bool retain)
{
    char topic[96]; snprintf(topic, sizeof(topic), "%s/%s", base, leaf);
    return publish(topic, payload, retain);
}

static void brightness_command(MessageData *data)
{
    MQTTMessage *m = data->message;
    /* Controls are never retained; ignore replayed commands from old clients. */
    if (m->retained || m->payloadlen != 1) return;
    char c = *(char *)m->payload;
    if (c >= '0' && c <= '4') m1_set_brightness(c - '0');
}

static void feature_command(MessageData *d)
{
    MQTTString *t = d->topicName;
    char topic[FEATURE_TOPIC_MAX];
    unsigned n = t->cstring ? strlen(t->cstring) : t->lenstring.len;
    if (n >= sizeof(topic)) return;
    memcpy(topic, t->cstring ? t->cstring : t->lenstring.data, n); topic[n] = 0;
    feature_mqtt_input(topic, d->message->payload, d->message->payloadlen, d->message->retained);
}
static char feature_filters[12][FEATURE_TOPIC_MAX];
static void native_command(MessageData *d)
{
    MQTTString *t=d->topicName; char topic[FEATURE_TOPIC_MAX];
    unsigned n=t->cstring?strlen(t->cstring):t->lenstring.len;
    if(n>=sizeof(topic))return;
    memcpy(topic,t->cstring?t->cstring:t->lenstring.data,n);topic[n]=0;
    if(feature_mqtt_input(topic,d->message->payload,d->message->payloadlen,d->message->retained))return;
    if(!strcmp(topic,command_topic))brightness_command(d);
}
static void ha_birth(MessageData *data)
{
    if (data->message->payloadlen == 6 && !memcmp(data->message->payload, "online", 6))
        rediscover = true;
}

static void str(json_object *o, const char *key, const char *value)
{ json_object_object_add(o, key, json_object_new_string(value)); }

static int discovery(const m1_config_t *c)
{
    static const char *keys[] = {"temperature", "humidity", "formaldehyde", "pm25", "brightness"};
    static const char *names[] = {"温度", "湿度", "甲醛", "PM2.5", "屏幕亮度"};
    static const char *units[] = {"°C", "%", "mg/m³", "µg/m³"};
    static const char *classes[] = {"temperature", "humidity", NULL, "pm25"};
    for (unsigned i = 0; i < 5; i++) {
        if (feature_bound()) { char path[128]; snprintf(path,sizeof(path),"homeassistant/%s/m1_%s/%s/config",i<4?"sensor":"number",m1_id,keys[i]); int rc=publish(path,"",true); if(rc!=MQTT_SUCCESS)return rc; continue; }
        char topic[128], text[128];
        json_object *o = json_object_new_object();
        if (!o) return MQTT_FAILURE;
        str(o, "name", names[i]);
        snprintf(text, sizeof(text), "m1_%s_%s", m1_id, keys[i]); str(o, "unique_id", text);
        json_object *device = json_object_new_object(), *ids = json_object_new_array();
        if (!device || !ids) {
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            json_object_put(o); return MQTT_FAILURE;
        }
        snprintf(text, sizeof(text), "m1_%s", m1_id);
        json_object_array_add(ids, json_object_new_string(text));
        json_object_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "M1 EMW3080B");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", M1_VERSION);
        json_object_object_add(o, "device", device);
        if (i < 4) {
            snprintf(text, sizeof(text), "%s/sensor", base); str(o, "state_topic", text);
            snprintf(text, sizeof(text), "{{ value_json.%s }}", keys[i]); str(o, "value_template", text);
            str(o, "unit_of_measurement", units[i]); str(o, "state_class", "measurement");
            if (classes[i]) str(o, "device_class", classes[i]);
            json_object_object_add(o, "expire_after", json_object_new_int(15));
            json_object *availability = json_object_new_array();
            json_object *node = json_object_new_object(), *sensor = json_object_new_object();
            if (!availability || !node || !sensor) {
                if (availability) json_object_put(availability);
                if (node) json_object_put(node);
                if (sensor) json_object_put(sensor);
                json_object_put(o); return MQTT_FAILURE;
            }
            str(node, "topic", availability_topic);
            snprintf(text, sizeof(text), "%s/sensor_availability", base); str(sensor, "topic", text);
            json_object_array_add(availability, node); json_object_array_add(availability, sensor);
            json_object_object_add(o, "availability", availability);
            str(o, "availability_mode", "all");
        } else {
            str(o, "command_topic", command_topic);
            snprintf(text, sizeof(text), "%s/brightness/state", base); str(o, "state_topic", text);
            str(o, "availability_topic", availability_topic);
            json_object_object_add(o, "min", json_object_new_int(0));
            json_object_object_add(o, "max", json_object_new_int(4));
            json_object_object_add(o, "step", json_object_new_int(1));
            str(o, "mode", "slider");
        }
        snprintf(topic, sizeof(topic), "homeassistant/%s/m1_%s/%s/config",
                 i < 4 ? "sensor" : "number", m1_id, keys[i]);
        int rc = publish(topic, json_object_to_json_string(o), true);
        json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}

static bool same_connection(const m1_config_t *a, const m1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) &&
        !strcmp(a->name, b->name);
}

static void mqtt_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    char client_id[24], wildcard[96]; snprintf(client_id, sizeof(client_id), "m1_%s", m1_id);
    snprintf(base, sizeof(base), "phicomm/m1/%s", m1_id);
    snprintf(wildcard,sizeof(wildcard),"%s/#",base);
    snprintf(command_topic, sizeof(command_topic), "%s/brightness/set", base);
    snprintf(availability_topic, sizeof(availability_topic), "%s/availability", base);
    for (;;) {
        m1_config_t cfg, current;
        m1_state_t state;
        m1_config_get(&cfg); m1_state_get(&state);
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mico_thread_msleep(1000); continue; }
        Network net = {0}; net.my_socket = -1;
        memset(&client, 0, sizeof(client));
        ssl_opts options = {0};
        if (NewNetwork(&net, cfg.mqtt_host, cfg.mqtt_port, options) != MQTT_SUCCESS) {
            mico_thread_msleep(3000); continue;
        }
        if (MQTTClientInit(&client, &net, 5000) != MQTT_SUCCESS) {
            net.disconnect(&net); mico_thread_msleep(3000); continue;
        }
        MQTTPacket_connectData connect = MQTTPacket_connectData_initializer;
        connect.MQTTVersion = 4; connect.clientID.cstring = client_id;
        connect.username.cstring = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL;
        connect.password.cstring = cfg.mqtt_user[0] ? cfg.mqtt_password : NULL;
        connect.keepAliveInterval = 30; connect.cleansession = 1;
        connect.willFlag = 1; connect.will.topicName.cstring = availability_topic;
        connect.will.message.cstring = "offline"; connect.will.retained = 1; connect.will.qos = 1;
        feature_config_t *fc = malloc(sizeof(*fc));
        if (!fc) { net.disconnect(&net); MQTTClientDeinit(&client); mico_thread_msleep(3000); continue; }
        feature_config_get(fc); uint32_t feature_revision=fc->revision; free(fc);
        bool connected = MQTTConnect(&client, &connect) == MQTT_SUCCESS;
        int rc = connected ? MQTTSubscribe(&client, wildcard, QOS0, native_command) : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS) rc = MQTTSubscribe(&client, "homeassistant/status", QOS0, ha_birth);
        unsigned count = feature_mqtt_filters(feature_filters);
        for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; i++) rc = MQTTSubscribe(&client, feature_filters[i], QOS0, feature_command);
        if (rc == MQTT_SUCCESS) rc = publish_leaf("sensor_availability", "offline", true);
        if (rc == MQTT_SUCCESS) rc = discovery(&cfg);
        if (rc == MQTT_SUCCESS) rc = publish(availability_topic, "online", true);
        uint32_t sequence = 0; int old_brightness = -1, old_fresh = -1;
        rediscover = false;
        while (rc == MQTT_SUCCESS) {
            mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_connected = true; mico_rtos_unlock_mutex(&m1_lock);
            m1_state_get(&state); m1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) break;
            fc=malloc(sizeof(*fc)); if (!fc) break; feature_config_get(fc);
            bool feature_changed=feature_revision!=fc->revision; free(fc); if(feature_changed)break;
            if (rediscover) {
                feature_mqtt_service(publish,false);
                rc = discovery(&current); rediscover = false;
                old_brightness = old_fresh = -1; sequence = 0;
            }
            bool fresh = m1_sample_fresh(&state.sample, mico_rtos_get_time());
            if (rc == MQTT_SUCCESS && fresh != old_fresh) {
                rc = publish_leaf("sensor_availability", fresh ? "online" : "offline", true);
                old_fresh = fresh;
            }
            if (rc == MQTT_SUCCESS && fresh && sequence != state.sample.sequence) {
                const m1_sample_t *s = &state.sample;
                char payload[192];
                snprintf(payload, sizeof(payload),
                    "{\"temperature\":%ld.%ld,\"humidity\":%ld.%ld,\"formaldehyde\":%u.%03u,\"pm25\":%u}",
                    (long)(s->temperature_deci_c / 10), (long)(s->temperature_deci_c % 10),
                    (long)(s->humidity_deci_pct / 10), (long)(s->humidity_deci_pct % 10),
                    s->hcho_ug_m3 / 1000, s->hcho_ug_m3 % 1000, s->pm25_ug_m3);
                rc = publish_leaf("sensor", payload, false);
                sequence = state.sample.sequence;
            }
            if (rc == MQTT_SUCCESS && old_brightness != state.brightness_requested) {
                char value[4]; snprintf(value, sizeof(value), "%u", state.brightness_requested);
                rc = publish_leaf("brightness/state", value, true);
                old_brightness = state.brightness_requested;
            }
            if (rc == MQTT_SUCCESS) feature_mqtt_service(publish, true);
            if (rc == MQTT_SUCCESS) rc = MQTTYield(&client, 100);
            mico_thread_msleep(50);
        }
        mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&m1_lock);
        feature_mqtt_service(publish, false);
        if (connected) {
            publish(availability_topic, "offline", true);
            MQTTDisconnect(&client);
        }
        net.disconnect(&net); MQTTClientDeinit(&client);
        mico_thread_msleep(3000);
    }
}

OSStatus m1_mqtt_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_mqtt", mqtt_worker, 8192, 0); }
