#include "m1.h"
#include "MQTTClient.h"
#include "../common/mqtt_cleanup.h"

int m1_mqtt_read(Network *, unsigned char *, int, int);

static Client client;
static mqtt_account_t account;
static char base[192], command_topic[MQTT_TOPIC_MAX], availability_topic[MQTT_TOPIC_MAX];
static bool rediscover, cleanup_pending;
static m1_config_t session_config;
static bool same_connection(const m1_config_t *, const m1_config_t *);
static bool session_current(void)
{ m1_config_t c; m1_config_get(&c); return same_connection(&session_config, &c); }
static void mqtt_status(unsigned status)
{ mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_status = status; m1_state.mqtt_connected = status == MQTT_READY; mico_rtos_unlock_mutex(&m1_lock); }

static void mqtt_error(const char *operation, int result)
{
    if (result == MQTT_SUCCESS) return;
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.mqtt_errors++;
    snprintf(m1_state.mqtt_last_error, sizeof(m1_state.mqtt_last_error), "%s: %d", operation, result);
    mico_rtos_unlock_mutex(&m1_lock);
}

static int publish(const char *topic, const char *payload, bool retain)
{
    MQTTMessage message = {0};
    message.qos = account.mode || !strncmp(topic,"homeassistant/",14) ? QOS1 : QOS0; message.retained = retain;
    message.payload = (void *)payload; message.payloadlen = strlen(payload);
    int result = MQTTPublish(&client, topic, &message);
    mqtt_error(topic, result);
    return result;
}

static int publish_leaf(const char *leaf, const char *payload, bool retain)
{
    char topic[MQTT_TOPIC_MAX]; snprintf(topic, sizeof(topic), "%s/%s", base, leaf);
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
    if (!session_current()) return;
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
    if (!session_current()) return;
    MQTTString *t=d->topicName; char topic[FEATURE_TOPIC_MAX];
    unsigned n=t->cstring?strlen(t->cstring):t->lenstring.len;
    if(n>=sizeof(topic))return;
    memcpy(topic,t->cstring?t->cstring:t->lenstring.data,n);topic[n]=0;
    if(feature_mqtt_input(topic,d->message->payload,d->message->payloadlen,d->message->retained))return;
    if(!strcmp(topic,command_topic))brightness_command(d);
}
static void ha_birth(MessageData *data)
{
    if (!session_current()) return;
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
        if (feature_bound()) { char path[MQTT_TOPIC_MAX]; mqtt_ha(&account, i<4?"sensor":"number", keys[i], path, sizeof(path)); int rc=publish(path,"",true); if(rc!=MQTT_SUCCESS)return rc; continue; }
        char topic[MQTT_TOPIC_MAX], text[MQTT_TOPIC_MAX];
        json_object *o = json_object_new_object();
        if (!o) return MQTT_FAILURE;
        str(o, "name", names[i]);
        json_object_object_add(o, "retain", json_object_new_boolean(false));
        mqtt_identity(&account, keys[i], text, sizeof(text)); str(o, "unique_id", text);
        json_object *device = json_object_new_object(), *ids = json_object_new_array();
        if (!device || !ids) {
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            json_object_put(o); return MQTT_FAILURE;
        }
        mqtt_identity(&account, "", text, sizeof(text));
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
        mqtt_ha(&account, i < 4 ? "sensor" : "number", keys[i], topic, sizeof(topic));
        int rc = publish(topic, json_object_to_json_string(o), true);
        json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}

static int cleanup_discovery(void)
{
    const char *keys[] = {"temperature","humidity","formaldehyde","pm25","brightness"};
    const char *components[] = {"sensor","sensor","sensor","sensor","number"};
    int result = MQTT_SUCCESS;
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        char path[MQTT_TOPIC_MAX]; mqtt_ha(&account, components[i], keys[i], path, sizeof(path));
        if (publish(path, "", true) != MQTT_SUCCESS) result = MQTT_FAILURE;
    }
    return result;
}
static int cleanup_all(void)
{
    int result = cleanup_discovery();
    if (feature_mqtt_cleanup(publish) != MQTT_SUCCESS) result = MQTT_FAILURE;
    return result;
}
static bool same_connection(const m1_config_t *a, const m1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) &&
        !strcmp(a->name, b->name) && a->mqtt_topic_mode == b->mqtt_topic_mode;
}

static void mqtt_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    char client_id[192], wildcard[MQTT_TOPIC_MAX];
    unsigned attempt = 0;
    for (;;) {
        m1_config_t cfg, current;
        m1_state_t state;
        m1_config_get(&cfg); m1_state_get(&state);
        if (cleanup_pending && state.wifi_connected && !same_connection(&session_config, &cfg)) {
            int result = mqtt_cleanup_reconnect(&client, &account, session_config.mqtt_host,
                session_config.mqtt_port, session_config.mqtt_user, session_config.mqtt_password, cleanup_all);
            mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_cleanup_warning = result != MQTT_SUCCESS; mico_rtos_unlock_mutex(&m1_lock);
            cleanup_pending = false;
        }
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(1000); continue; }
        if (!mqtt_account_init(&account, cfg.mqtt_topic_mode, "m1", cfg.mqtt_user, m1_id)) {
            mqtt_status(MQTT_CONFIG_ERROR); mico_thread_msleep(1000); continue;
        }
        strcpy(base, account.base);
        mqtt_identity(&account, "", client_id, sizeof(client_id));
        mqtt_leaf(&account, "#", wildcard, sizeof(wildcard));
        mqtt_leaf(&account, "availability", availability_topic, sizeof(availability_topic));
        mqtt_leaf(&account, "brightness/set", command_topic, sizeof(command_topic));
        session_config = cfg;
        feature_mqtt_session(&account);
        mqtt_status(MQTT_CONNECTING);
        Network net = {0}; net.my_socket = -1;
        memset(&client, 0, sizeof(client));
        ssl_opts options = {0};
        if (NewNetwork(&net, cfg.mqtt_host, cfg.mqtt_port, options) != MQTT_SUCCESS) {
            mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue;
        }
        net.mqttread = m1_mqtt_read;
        if (MQTTClientInit(&client, &net, 5000) != MQTT_SUCCESS) {
            net.disconnect(&net); mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue;
        }
        MQTTPacket_connectData connect = MQTTPacket_connectData_initializer;
        connect.MQTTVersion = 4; connect.clientID.cstring = client_id;
        connect.username.cstring = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL;
        connect.password.cstring = cfg.mqtt_user[0] ? cfg.mqtt_password : NULL;
        connect.keepAliveInterval = 30; connect.cleansession = 1;
        connect.willFlag = 1; connect.will.topicName.cstring = availability_topic;
        connect.will.message.cstring = "offline"; connect.will.retained = 1; connect.will.qos = 1;
        feature_config_t *fc = malloc(sizeof(*fc));
        if (!fc) { net.disconnect(&net); MQTTClientDeinit(&client); mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue; }
        feature_config_get(fc); uint32_t feature_revision=fc->revision; free(fc);
        int connect_result = MQTTConnect(&client, &connect);
        bool connected = connect_result == MQTT_SUCCESS;
        if (!connected) mqtt_status(connect_result == 4 || connect_result == 5 ? MQTT_AUTH_FAILED : MQTT_DISCONNECTED);
        int announced = MQTT_SUCCESS;
        if (connected && account.mode) {
            char topic[MQTT_TOPIC_MAX]; mqtt_aux(&account, "discovery", account.mac, topic, sizeof(topic));
            announced = publish(topic, mqtt_announcement(&account), true);
        }
        int rc = connected && announced == MQTT_SUCCESS ? MQTTSubscribe(&client, wildcard, account.mode ? QOS1 : QOS0, native_command) : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS) rc = MQTTSubscribe(&client, "homeassistant/status", QOS0, ha_birth);
        unsigned count = feature_mqtt_filters(feature_filters);
        for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; i++) rc = MQTTSubscribe(&client, feature_filters[i], account.mode ? QOS1 : QOS0, feature_command);
        if (rc == MQTT_SUCCESS) rc = publish_leaf("sensor_availability", "offline", true);
        if (connected && rc != MQTT_SUCCESS) mqtt_status(MQTT_WAITING_ACL);
        if (rc == MQTT_SUCCESS) { cleanup_pending = true; rc = discovery(&cfg); }
        if (rc == MQTT_SUCCESS) {
            char value[4]; snprintf(value, sizeof(value), "%u", state.brightness_requested);
            rc = publish_leaf("brightness/state", value, true);
            if (rc == MQTT_SUCCESS && account.mode) rc = publish_leaf("sensor", "", true);
        }
        if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
        if (rc == MQTT_SUCCESS) rc = publish(availability_topic, "online", true);
        if (rc == MQTT_SUCCESS) { attempt = 0; cleanup_pending = true; mqtt_status(MQTT_READY); }
        uint32_t acl_check = mico_rtos_get_time();
        uint32_t connect_sequence = state.sample.sequence;
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
            bool fresh = m1_sample_fresh(&state.sample, mico_rtos_get_time()) &&
                (!account.mode || state.sample.sequence != connect_sequence);
            if (rc == MQTT_SUCCESS && fresh != old_fresh) {
                rc = publish_leaf("sensor_availability", fresh ? "online" : "offline", true);
                if (rc == MQTT_SUCCESS && !fresh && account.mode) rc = publish_leaf("sensor", "", true);
                old_fresh = fresh;
            }
            if (rc == MQTT_SUCCESS && fresh && sequence != state.sample.sequence) {
                const m1_sample_t *s = &state.sample;
                char payload[192];
                snprintf(payload, sizeof(payload),
                    "{\"temperature\":%s%ld.%ld,\"humidity\":%s%ld.%ld,\"formaldehyde\":%u.%03u,\"pm25\":%u}",
                    s->temperature_deci_c < 0 ? "-" : "", labs((long)s->temperature_deci_c) / 10, labs((long)s->temperature_deci_c) % 10,
                    s->humidity_deci_pct < 0 ? "-" : "", labs((long)s->humidity_deci_pct) / 10, labs((long)s->humidity_deci_pct) % 10,
                    s->hcho_ug_m3 / 1000, s->hcho_ug_m3 % 1000, s->pm25_ug_m3);
                rc = publish_leaf("sensor", payload, account.mode);
                sequence = state.sample.sequence;
            }
            if (rc == MQTT_SUCCESS && old_brightness != state.brightness_requested) {
                char value[4]; snprintf(value, sizeof(value), "%u", state.brightness_requested);
                rc = publish_leaf("brightness/state", value, true);
                old_brightness = state.brightness_requested;
            }
            if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
            if (rc == MQTT_SUCCESS && account.mode && (uint32_t)(mico_rtos_get_time() - acl_check) >= 30000) {
                rc = MQTTSubscribe(&client, wildcard, QOS1, native_command);
                for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; ++i)
                    rc = MQTTSubscribe(&client, feature_filters[i], QOS1, feature_command);
                acl_check = mico_rtos_get_time();
                mqtt_error("ACL subscription check", rc);
                if (rc != MQTT_SUCCESS) mqtt_status(MQTT_WAITING_ACL);
            }
            if (rc == MQTT_SUCCESS) { rc = MQTTYield(&client, 100); mqtt_error("receive", rc); }
            mico_thread_msleep(50);
        }
        mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&m1_lock);
        m1_config_get(&current);
        if (connected && !same_connection(&cfg, &current)) {
            int cleanup = cleanup_all();
            cleanup_pending = false;
            mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_cleanup_warning = cleanup != MQTT_SUCCESS; mico_rtos_unlock_mutex(&m1_lock);
        }
        feature_mqtt_service(publish, false);
        if (connected) {
            publish(availability_topic, "offline", true);
            MQTTDisconnect(&client);
        }
        m1_state_get(&state);
        if (state.mqtt_status == MQTT_READY) mqtt_status(MQTT_DISCONNECTED);
        net.disconnect(&net); MQTTClientDeinit(&client);
        mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000);
    }
}

OSStatus m1_mqtt_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_mqtt", mqtt_worker, 12288, 0); }
