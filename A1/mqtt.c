#include "../common/ota/mode.h"
#include "a1.h"
#include "MQTTClient.h"
#include "../common/mqtt_transport.h"
#include "../common/mqtt_cleanup.h"
#include <ctype.h>

typedef struct { bool wifi_connected,led_visible; uint8_t mqtt_status; } mqtt_state_t;
static void mqtt_state_get(mqtt_state_t *out)
{
    mico_rtos_lock_mutex(&a1_lock);
    out->wifi_connected=a1_state.wifi_connected;out->led_visible=a1_state.led_visible;
    out->mqtt_status=a1_state.mqtt_status;
    mico_rtos_unlock_mutex(&a1_lock);
}

static Client client;
static mqtt_account_t account;
static char base[192], availability_topic[MQTT_TOPIC_MAX];
static bool rediscover, cleanup_pending;
static a1_config_t session_config;
static bool same_connection(const a1_config_t *, const a1_config_t *);
static bool session_current(void)
{ a1_config_t c; a1_config_get(&c); return same_connection(&session_config, &c); }
static void mqtt_status(unsigned status)
{ mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_status = status; a1_state.mqtt_connected = status == MQTT_READY; mico_rtos_unlock_mutex(&a1_lock); }

static void mqtt_error(const char *operation, int result)
{
    if (result == MQTT_SUCCESS) return;
    mico_rtos_lock_mutex(&a1_lock);
    a1_state.mqtt_errors++;
    snprintf(a1_state.mqtt_last_error, sizeof(a1_state.mqtt_last_error), "%s: %d", operation, result);
    mico_rtos_unlock_mutex(&a1_lock);
}

static int publish(const char *topic, const char *payload, bool retain)
{
    if (!payload) { mqtt_error("serialize", MQTT_FAILURE); return MQTT_FAILURE; }
    MQTTMessage m = {0}; m.qos = QOS1; m.retained = retain;
    m.payload = (void *)payload; m.payloadlen = strlen(payload);
    int result = MQTTPublish(&client, topic, &m);
    mqtt_error("publish", result);
    return result;
}
static int leaf(const char *name, const char *payload)
{
    char topic[MQTT_TOPIC_MAX]; snprintf(topic, sizeof(topic), "%s/%s", base, name);
    return publish(topic, payload, true);
}
static bool topic_is(MessageData *d, const char *suffix)
{
    char topic[MQTT_TOPIC_MAX]; snprintf(topic, sizeof(topic), "%s/%s", base, suffix);
    MQTTString *t = d->topicName;
    if (t->cstring) return !strcmp(t->cstring, topic);
    return t->lenstring.len == (int)strlen(topic) && !memcmp(t->lenstring.data, topic, t->lenstring.len);
}
#include "../common/mqtt_service.inc"
static bool ota_launch(const char *url) { return a1_ota_download(url); }
static void ota_snapshot(char *state,char *error,unsigned *progress) {
    mico_rtos_lock_mutex(&a1_lock);snprintf(state,32,"%s",a1_state.ota_state);
    snprintf(error,128,"%s",a1_state.ota_error);*progress=a1_state.ota_progress;
    mico_rtos_unlock_mutex(&a1_lock);
}
static int ota_publish(const char *leaf,const char *text,bool retained) {
    char topic[MQTT_TOPIC_MAX];snprintf(topic,sizeof(topic),"%s/%s",base,leaf);return publish(topic,text,retained);
}
#include "../common/ota/mqtt_ota.inc"
static void command(MessageData *d)
{
    if (!session_current()) return;
    MQTTMessage *m = d->message;
    if(topic_is(d,"command")){service_mqtt_input(publish,&account,m->payload,m->payloadlen,m->retained);return;}
    if(topic_is(d,"ota/set")){ota_command(base,m->payload,m->payloadlen,m->retained);return;}
    if(ota_job_active())return;
    if (m->retained || !m->payloadlen || m->payloadlen > A1_JSON_LIMIT) return;
    MQTTString *ft=d->topicName; char ftopic[FEATURE_TOPIC_MAX];
    unsigned fn=ft->cstring?strlen(ft->cstring):ft->lenstring.len;
    if(fn<sizeof(ftopic)){memcpy(ftopic,ft->cstring?ft->cstring:ft->lenstring.data,fn);ftopic[fn]=0;if(feature_mqtt_input(ftopic,m->payload,m->payloadlen,m->retained))return;}
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
            } else json_build_object_add(o, key, json_object_new_int(n));
        } else if (o && (!strcmp(text, "ON") || !strcmp(text, "OFF")))
            json_build_object_add(o, key, json_object_new_boolean(!strcmp(text, "ON")));
        else if (o) { json_object_put(o); o = NULL; }
    }
    if (o) { a1_control_local(kind, o); json_object_put(o); }
    free(text);
}
static void feature_command(MessageData *d)
{
    if(ota_job_active())return;
    if (!session_current()) return;
    MQTTString *t = d->topicName;
    char topic[FEATURE_TOPIC_MAX];
    unsigned n = t->cstring ? strlen(t->cstring) : t->lenstring.len;
    if (n >= sizeof(topic)) return;
    memcpy(topic, t->cstring ? t->cstring : t->lenstring.data, n); topic[n] = 0;
    feature_mqtt_input(topic, d->message->payload, d->message->payloadlen, d->message->retained);
}
static char feature_filters[FEATURE_MQTT_A1_FILTER_MAX][FEATURE_TOPIC_MAX];
static void ha_birth(MessageData *d)
{
    if(ota_job_active())return;
    if (!session_current()) return;
    if (d->message->payloadlen == 6 && !memcmp(d->message->payload, "online", 6)) rediscover = true;
}
static void str(json_object *o, const char *key, const char *value)
{ json_build_object_add(o, key, json_object_new_string(value)); }
static void topic(json_object *o, const char *key, const char *suffix)
{
    char value[MQTT_TOPIC_MAX]; snprintf(value, sizeof(value), "%s/%s", base, suffix); str(o, key, value);
}
/* Keep this frame visible to the production MQTT stack-budget check. */
static __attribute__((noinline)) int filter_discovery(const a1_config_t *c, bool clear)
{
    static const char *keys[] = {"filter_remaining", "filter_used", "filter_replacement", "filter_lifetime", "filter_reset", "fan_mode"};
    static const char *components[] = {"sensor", "sensor", "binary_sensor", "number", "button", "select"};
    static const char *names[] = {"滤芯剩余寿命", "滤芯使用时间", "滤芯更换提醒", "滤芯额定寿命", "更换滤芯后重置", "运行模式"};
    bool bound = feature_bound(), grouped = false;
    /* MQTT-owner scratch: PUBACK waiting can dispatch nested commands. */
    static char parent_id[192], parent_name[FEATURE_TOPIC_MAX];
    if (!clear && bound && !feature_ha_device(parent_id, sizeof(parent_id),
                                             parent_name, sizeof(parent_name), &grouped))
        return MQTT_FAILURE;
    for (unsigned i = 0; i < 6; ++i) {
        static char path[MQTT_TOPIC_MAX], identity[MQTT_TOPIC_MAX];
        mqtt_ha(&account, components[i], keys[i], path, sizeof(path));
        /* Wear belongs to this physical A1 even when fan control is grouped. */
        if (clear) { if (publish(path, "", true) != MQTT_SUCCESS) return MQTT_FAILURE; continue; }
        json_object *o = json_object_new_object(), *device = json_object_new_object(), *ids = json_object_new_array();
        if (!o || !device || !ids) {
            if (o) json_object_put(o);
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            return MQTT_FAILURE;
        }
        mqtt_identity(&account, "", identity, sizeof(identity));
        json_build_array_add(ids, json_object_new_string(bound ? parent_id : identity));
        json_build_object_add(device, "identifiers", ids);
        str(device, "name", bound ? parent_name : c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_build_object_add(o, "device", device);
        if (bound && grouped) {
            static char label[160];
            snprintf(label, sizeof(label), "%s %s", c->name, names[i]);
            str(o, "name", label);
        } else str(o, "name", names[i]);
        mqtt_identity(&account, keys[i], identity, sizeof(identity)); str(o, "unique_id", identity);
        str(o, "availability_topic", availability_topic); str(o, "icon", i == 5 ? "mdi:fan-auto" : "mdi:air-filter");
        if (i < 5) str(o, "entity_category", i < 3 ? "diagnostic" : "config");
        json_build_object_add(o, "retain", json_object_new_boolean(false));
        topic(o, "json_attributes_topic", "state");
        str(o, "json_attributes_template", "{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"config_dirty\": value_json.config_dirty} | tojson }}");
        if (i < 4 || i == 5) topic(o, "state_topic", "state");
        if (i == 0) {
            str(o, "value_template", "{{ value_json.filter.remaining_percent }}");
            str(o, "unit_of_measurement", "%"); str(o, "state_class", "measurement");
        } else if (i == 1) {
            str(o, "value_template", "{{ (value_json.filter.used_seconds / 3600) | round(1) }}");
            str(o, "unit_of_measurement", "h"); str(o, "device_class", "duration");
        } else if (i == 2) {
            str(o, "value_template", "{{ 'ON' if value_json.filter.replacement_due else 'OFF' }}");
            str(o, "device_class", "problem");
        } else if (i == 3) {
            str(o, "value_template", "{{ value_json.filter.lifetime_hours }}");
            topic(o, "command_topic", "command");
            str(o, "command_template", "{\"action\":\"filter.set\",\"params\":{\"lifetime_hours\":{{ value | int }}}}");
            str(o, "unit_of_measurement", "h"); str(o, "mode", "box");
            json_build_object_add(o, "min", json_object_new_int(1));
            json_build_object_add(o, "max", json_object_new_int(A1_FILTER_MAX_HOURS));
            json_build_object_add(o, "step", json_object_new_int(1));
        } else if (i == 4) {
            topic(o, "command_topic", "command");
            str(o, "payload_press", "{\"action\":\"filter.reset\",\"params\":{}}");
        } else {
            topic(o, "command_topic", "command");
            str(o, "value_template", "{{ '自动' if value_json.fan_mode == 'auto' else '手动' }}");
            str(o, "command_template", "{\"action\":\"fan.mode.set\",\"params\":{\"mode\":\"{{ 'auto' if value == '自动' else 'manual' }}\"}}");
            json_object *options = json_object_new_array();
            json_build_array_add(options, json_object_new_string("自动"));
            json_build_array_add(options, json_object_new_string("手动"));
            json_build_object_add(o, "options", options);
        }
        int rc = publish(path, json_object_to_json_string(o), true); json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}
static int discovery(const a1_config_t *c)
{
    const char *keys[] = {"fan", "light", "child_lock", "led_always_on"};
    const char *components[] = {"fan", "light", "switch", "switch"};
    const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮"};
    for (unsigned i = 0; i < 4; i++) {
        if (feature_bound()) { char path[MQTT_TOPIC_MAX]; mqtt_ha(&account, components[i], keys[i], path, sizeof(path)); int rc=publish(path,"",true); if(rc!=MQTT_SUCCESS)return rc; continue; }
        json_object *o = json_object_new_object(), *device = json_object_new_object(), *ids = json_object_new_array();
        if (!o || !device || !ids) {
            if (o) json_object_put(o);
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            return MQTT_FAILURE;
        }
        char text[MQTT_TOPIC_MAX], path[MQTT_TOPIC_MAX];
        mqtt_identity(&account, "", text, sizeof(text));
        json_build_array_add(ids, json_object_new_string(text));
        json_build_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_build_object_add(o, "device", device); str(o, "name", names[i]);
        mqtt_identity(&account, keys[i], text, sizeof(text)); str(o, "unique_id", text);
        str(o, "availability_topic", availability_topic);
        topic(o,"json_attributes_topic","state");
        str(o,"json_attributes_template","{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"config_dirty\": value_json.config_dirty} | tojson }}");
        json_build_object_add(o, "retain", json_object_new_boolean(false));
        topic(o, "command_topic", "command");
        snprintf(text, sizeof(text), "%s/state", keys[i]); topic(o, "state_topic", text);
        if (i == 0) {
            str(o, "icon", "mdi:air-purifier");
            topic(o,"preset_mode_command_topic","command");
            topic(o,"preset_mode_state_topic","fan/mode/state");
            str(o,"preset_mode_command_template","{\"action\":\"fan.mode.set\",\"params\":{\"mode\":{{ value | tojson }}}}");
            json_object *modes=json_object_new_array();
            json_build_array_add(modes,json_object_new_string("auto"));
            json_build_array_add(modes,json_object_new_string("manual"));
            json_build_object_add(o,"preset_modes",modes);
            topic(o, "percentage_command_topic", "command");
            str(o,"command_template","{\"action\":\"fan.set\",\"params\":{\"on\":{{ 'true' if value == 'ON' else 'false' }}}}");
            str(o,"percentage_command_template","{\"action\":\"fan.set\",\"params\":{\"mode\":\"manual\",\"speed\":{{ value | int }}}}");
            topic(o, "percentage_state_topic", "fan/percentage/state");
            json_build_object_add(o, "speed_range_min", json_object_new_int(1));
            json_build_object_add(o, "speed_range_max", json_object_new_int(100));
        }
        if (i == 1) {
            str(o,"schema","template");
            str(o,"command_on_template","{\"action\":\"light.set\",\"params\":{\"enabled\":true{% if brightness is defined %},\"brightness\":{{ ((brightness | int) * 100 / 255) | round | int }}{% endif %}{% if red is defined %},\"color\":{\"r\":{{ red }},\"g\":{{ green }},\"b\":{{ blue }}}{% endif %}}}");
            str(o,"command_off_template","{\"action\":\"light.set\",\"params\":{\"enabled\":false}}");
            str(o,"state_template","{{ value_json.state | lower }}");
            str(o,"brightness_template","{{ value_json.brightness }}");
            str(o,"red_template","{{ value_json.color.r }}");
            str(o,"green_template","{{ value_json.color.g }}");
            str(o,"blue_template","{{ value_json.color.b }}");
        } else if(i==0) { str(o,"payload_on","ON");str(o,"payload_off","OFF"); }
        else {
            const char *on=i==2?"{\"action\":\"child_lock.set\",\"params\":{\"enabled\":true}}":"{\"action\":\"light.set\",\"params\":{\"always_on\":true}}";
            const char *off=i==2?"{\"action\":\"child_lock.set\",\"params\":{\"enabled\":false}}":"{\"action\":\"light.set\",\"params\":{\"always_on\":false}}";
            str(o,"payload_on",on);str(o,"payload_off",off);str(o,"state_on","ON");str(o,"state_off","OFF");
        }
        if (i >= 2) str(o, "entity_category", "config");
        mqtt_ha(&account, components[i], keys[i], path, sizeof(path));
        int rc = publish(path, json_object_to_json_string(o), true); json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return filter_discovery(c, false);
}
static int states(const a1_config_t *c, const mqtt_state_t *s)
{
    bool on; unsigned desired;feature_effective_fan(c->fan_on,c->speed,&on,&desired);
    int rc = leaf("fan/state", on ? "ON" : "OFF");
    char speed[4], light[192]; snprintf(speed, sizeof(speed), "%u", on ? desired : 0);
    if (rc == MQTT_SUCCESS) rc = leaf("fan/percentage/state", speed);
    if (rc == MQTT_SUCCESS) rc = leaf("fan/mode/state",feature_fan_auto()?"auto":"manual");
    snprintf(light, sizeof(light), "{\"state\":\"%s\",\"color_mode\":\"rgb\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
        s->led_visible ? "ON" : "OFF", c->brightness, c->red, c->green, c->blue);
    if (rc == MQTT_SUCCESS) rc = leaf("light/state", light);
    if (rc == MQTT_SUCCESS) rc = leaf("child_lock/state", c->child_lock ? "ON" : "OFF");
    if (rc == MQTT_SUCCESS) rc = leaf("led_always_on/state", c->led_always_on ? "ON" : "OFF");
    if(rc==MQTT_SUCCESS)rc=service_mqtt_snapshot(publish,&account);
    return rc;
}
static int cleanup_discovery(void)
{
    const char *keys[] = {"fan","light","child_lock","led_always_on"};
    const char *components[] = {"fan","light","switch","switch"};
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
    if (filter_discovery(&session_config, true) != MQTT_SUCCESS) result = MQTT_FAILURE;
    if (feature_mqtt_cleanup(publish) != MQTT_SUCCESS) result = MQTT_FAILURE;
    return result;
}
static bool same_connection(const a1_config_t *a, const a1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) && !strcmp(a->name, b->name);
}
static void mqtt_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    char client_id[192], wildcard[MQTT_TOPIC_MAX];
    unsigned attempt = 0;
    for (;;) {
        /* Only the MQTT owner uses these snapshots. Keep them off its nested
         * discovery/service call stack (over 4 KiB before JSON formatting). */
        static a1_config_t cfg, current; static mqtt_state_t state;
        a1_config_get(&cfg); mqtt_state_get(&state);
        if (cleanup_pending && state.wifi_connected && !same_connection(&session_config, &cfg)) {
            int result = mqtt_cleanup_reconnect(&client, &account, session_config.mqtt_host,
                session_config.mqtt_port, session_config.mqtt_user, session_config.mqtt_password, cleanup_all);
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_cleanup_warning = result != MQTT_SUCCESS; mico_rtos_unlock_mutex(&a1_lock);
            cleanup_pending = false;
        }
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(1000); continue; }
        if (!mqtt_account_init(&account, MQTT_TOPIC_TMCP, "a1", cfg.mqtt_user, a1_id)) {
            mqtt_status(MQTT_CONFIG_ERROR); mico_thread_msleep(1000); continue;
        }
        strcpy(base, account.base);
        mqtt_identity(&account, "", client_id, sizeof(client_id));
        mqtt_leaf(&account, "command", wildcard, sizeof(wildcard));
        mqtt_leaf(&account, "availability", availability_topic, sizeof(availability_topic));
        session_config = cfg;
        feature_mqtt_session(&account);
        mqtt_status(MQTT_CONNECTING);
        Network net = {0}; net.my_socket = -1; memset(&client, 0, sizeof(client));
        ssl_opts options = {0};
        if (NewNetwork(&net, cfg.mqtt_host, cfg.mqtt_port, options) != MQTT_SUCCESS) { mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue; }
        net.mqttread = mqtt_socket_read;
        if (MQTTClientInit(&client, &net, 5000) != MQTT_SUCCESS) { net.disconnect(&net); mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue; }
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
        if (connected) {
            char topic[MQTT_TOPIC_MAX]; mqtt_aux(&account, "discovery", account.mac, topic, sizeof(topic));
            announced = publish(topic, mqtt_announcement(&account), true);
        }
        /* HA birth can arrive while initial QoS1 discovery awaits PUBACK.
         * Preserve it until the owner loop services rediscovery. */
        rediscover = false;
        const char *stage="initial";
        int rc = connected && announced == MQTT_SUCCESS ? MQTTSubscribe(&client, wildcard, QOS1, command) : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS) rc = MQTTSubscribe(&client, "homeassistant/status", QOS0, ha_birth);
        if(rc==MQTT_SUCCESS)rc=service_mqtt_capabilities(publish,&account);
        unsigned count = feature_mqtt_filters(feature_filters,FEATURE_MQTT_A1_FILTER_MAX);
        if(count>FEATURE_MQTT_A1_FILTER_MAX)rc=MQTT_FAILURE;
        for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; i++) rc = MQTTSubscribe(&client, feature_filters[i], QOS1, feature_command);
        if (connected && rc != MQTT_SUCCESS) mqtt_status(MQTT_WAITING_ACL);
        if (rc == MQTT_SUCCESS) { cleanup_pending = true; rc = discovery(&cfg); }
        if (rc == MQTT_SUCCESS) rc = states(&cfg, &state);
        if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
        if (rc == MQTT_SUCCESS) rc = publish(availability_topic, "online", true);
        if (rc == MQTT_SUCCESS) { attempt = 0; cleanup_pending = true; mqtt_status(MQTT_READY); }
        uint32_t acl_check = mico_rtos_get_time();
        bool old_on=false;unsigned old_speed=0;
        bool sent = false, old_visible = false; uint32_t revision = 0;
        a1_filter_storage_t sent_filter = {0}; uint32_t filter_sent_at = 0;
        while (rc == MQTT_SUCCESS) {
            stage="result";rc=service_mqtt_flush(publish);if(rc!=MQTT_SUCCESS)break;
            stage="ota"; rc=ota_service(base);if(rc!=MQTT_SUCCESS)break;
            if(ota_job_active()){rc=service_mqtt_flush(publish);if(rc!=MQTT_SUCCESS)break;rc=MQTTYield(&client,100);mico_thread_msleep(50);continue;}
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = true; mico_rtos_unlock_mutex(&a1_lock);
            mqtt_state_get(&state); a1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) break;
            fc=malloc(sizeof(*fc)); if (!fc) break; feature_config_get(fc);
            bool feature_changed=feature_revision!=fc->revision;
            feature_revision=fc->revision; free(fc);
            if(feature_changed && !feature_mqtt_filters_current(feature_filters,count))break;
            if (rediscover) { feature_mqtt_service(publish,false);rc = discovery(&current); rediscover = false; sent = false; }
            bool on;unsigned speed;feature_effective_fan(current.fan_on,current.speed,&on,&speed);
            a1_filter_storage_t filter; a1_filter_get(&filter);
            if (rc == MQTT_SUCCESS && (!sent || feature_changed || revision != current.revision || old_visible != state.led_visible || old_on!=on || old_speed!=speed)) {
                stage="state"; rc = states(&current, &state); revision = current.revision; old_visible = state.led_visible;old_on=on;old_speed=speed; sent = true;
                if (rc == MQTT_SUCCESS) { sent_filter = filter; filter_sent_at = mico_rtos_get_time(); }
            } else if (rc == MQTT_SUCCESS && a1_filter_mqtt_due(&filter, &sent_filter, mico_rtos_get_time(), filter_sent_at)) {
                stage="filter"; rc = service_mqtt_snapshot(publish, &account);
                if (rc == MQTT_SUCCESS) { sent_filter = filter; filter_sent_at = mico_rtos_get_time(); }
            }
            if (rc == MQTT_SUCCESS) {stage="link";rc = feature_mqtt_service(publish, true);}
            if (rc == MQTT_SUCCESS) {stage="result";rc = service_mqtt_flush(publish);}
            if (rc == MQTT_SUCCESS && (uint32_t)(mico_rtos_get_time() - acl_check) >= 30000) {
                stage="subscribe";rc = MQTTSubscribe(&client, wildcard, QOS1, command);
                for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; ++i)
                    rc = MQTTSubscribe(&client, feature_filters[i], QOS1, feature_command);
                acl_check = mico_rtos_get_time();
                if (rc != MQTT_SUCCESS) { mqtt_error("subscription", rc); mqtt_status(MQTT_WAITING_ACL); }
            }
            if (rc == MQTT_SUCCESS) {stage="receive";rc = MQTTYield(&client, 100); mqtt_error("receive", rc);}
            if (rc == MQTT_SUCCESS) {stage="result";rc=service_mqtt_flush(publish);}
            mico_thread_msleep(50);
        }
        service_mqtt_clear();
        if (rc != MQTT_SUCCESS) {
            char event[40]; snprintf(event,sizeof(event),"%s_error_%d",stage,rc);
            service_event("warn","mqtt",event);
        }
        mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&a1_lock);
        a1_config_get(&current);
        if(!same_connection(&cfg,&current))ota_forget();
        if (connected && !same_connection(&cfg, &current)) {
            int cleanup = cleanup_all();
            cleanup_pending = false;
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_cleanup_warning = cleanup != MQTT_SUCCESS; mico_rtos_unlock_mutex(&a1_lock);
        }
        feature_mqtt_service(publish, false);
        if (connected) { publish(availability_topic, "offline", true); MQTTDisconnect(&client); }
        mqtt_state_get(&state);
        if (state.mqtt_status == MQTT_READY) mqtt_status(MQTT_DISCONNECTED);
        net.disconnect(&net); MQTTClientDeinit(&client); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000);
    }
}
OSStatus a1_mqtt_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_mqtt", mqtt_worker, 16384, 0); }
