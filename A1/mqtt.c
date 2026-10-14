#include "a1.h"
#include "MQTTClient.h"
#include "../common/mqtt_cleanup.h"
#include <ctype.h>

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

static int publish(const char *topic, const char *payload, bool retain)
{
    MQTTMessage m = {0}; m.qos = account.mode || !strncmp(topic,"homeassistant/",14) ? QOS1 : QOS0; m.retained = retain;
    m.payload = (void *)payload; m.payloadlen = strlen(payload);
    return MQTTPublish(&client, topic, &m);
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
    if(topic_is(d,"ota/set")){ota_command(base,m->payload,m->payloadlen,m->retained);return;}
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
            } else json_object_object_add(o, key, json_object_new_int(n));
        } else if (o && (!strcmp(text, "ON") || !strcmp(text, "OFF")))
            json_object_object_add(o, key, json_object_new_boolean(!strcmp(text, "ON")));
        else if (o) { json_object_put(o); o = NULL; }
    }
    if (o) { a1_control_local(kind, o); json_object_put(o); }
    free(text);
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
static void ha_birth(MessageData *d)
{
    if (!session_current()) return;
    if (d->message->payloadlen == 6 && !memcmp(d->message->payload, "online", 6)) rediscover = true;
}
static void str(json_object *o, const char *key, const char *value)
{ json_object_object_add(o, key, json_object_new_string(value)); }
static void topic(json_object *o, const char *key, const char *suffix)
{
    char value[MQTT_TOPIC_MAX]; snprintf(value, sizeof(value), "%s/%s", base, suffix); str(o, key, value);
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
        json_object_array_add(ids, json_object_new_string(text));
        json_object_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_object_object_add(o, "device", device); str(o, "name", names[i]);
        mqtt_identity(&account, keys[i], text, sizeof(text)); str(o, "unique_id", text);
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
        mqtt_ha(&account, components[i], keys[i], path, sizeof(path));
        int rc = publish(path, json_object_to_json_string(o), true); json_object_put(o);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}
static int states(const a1_config_t *c, const a1_state_t *s)
{
    bool on; unsigned desired;feature_effective_fan(c->fan_on,c->speed,&on,&desired);
    int rc = leaf("fan/state", on ? "ON" : "OFF");
    char speed[4], light[192]; snprintf(speed, sizeof(speed), "%u", on ? desired : 0);
    if (rc == MQTT_SUCCESS) rc = leaf("fan/percentage/state", speed);
    snprintf(light, sizeof(light), "{\"state\":\"%s\",\"color_mode\":\"rgb\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
        s->led_visible ? "ON" : "OFF", c->brightness, c->red, c->green, c->blue);
    if (rc == MQTT_SUCCESS) rc = leaf("light/state", light);
    if (rc == MQTT_SUCCESS) rc = leaf("child_lock/state", c->child_lock ? "ON" : "OFF");
    if (rc == MQTT_SUCCESS) rc = leaf("led_always_on/state", c->led_always_on ? "ON" : "OFF");
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
    if (feature_mqtt_cleanup(publish) != MQTT_SUCCESS) result = MQTT_FAILURE;
    return result;
}
static bool same_connection(const a1_config_t *a, const a1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) && !strcmp(a->name, b->name) && a->mqtt_topic_mode == b->mqtt_topic_mode;
}
static void mqtt_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    char client_id[192], wildcard[MQTT_TOPIC_MAX];
    unsigned attempt = 0;
    for (;;) {
        a1_config_t cfg, current; a1_state_t state;
        a1_config_get(&cfg); a1_state_get(&state);
        if (cleanup_pending && state.wifi_connected && !same_connection(&session_config, &cfg)) {
            int result = mqtt_cleanup_reconnect(&client, &account, session_config.mqtt_host,
                session_config.mqtt_port, session_config.mqtt_user, session_config.mqtt_password, cleanup_all);
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_cleanup_warning = result != MQTT_SUCCESS; mico_rtos_unlock_mutex(&a1_lock);
            cleanup_pending = false;
        }
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(1000); continue; }
        if (!mqtt_account_init(&account, cfg.mqtt_topic_mode, "a1", cfg.mqtt_user, a1_id)) {
            mqtt_status(MQTT_CONFIG_ERROR); mico_thread_msleep(1000); continue;
        }
        strcpy(base, account.base);
        mqtt_identity(&account, "", client_id, sizeof(client_id));
        if (!account.mode) snprintf(client_id, sizeof(client_id), "phicomm_a1_%s", a1_id);
        mqtt_leaf(&account, "#", wildcard, sizeof(wildcard));
        mqtt_leaf(&account, "availability", availability_topic, sizeof(availability_topic));
        session_config = cfg;
        feature_mqtt_session(&account);
        mqtt_status(MQTT_CONNECTING);
        Network net = {0}; net.my_socket = -1; memset(&client, 0, sizeof(client));
        ssl_opts options = {0};
        if (NewNetwork(&net, cfg.mqtt_host, cfg.mqtt_port, options) != MQTT_SUCCESS) { mqtt_status(MQTT_DISCONNECTED); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000); continue; }
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
        if (connected && account.mode) {
            char topic[MQTT_TOPIC_MAX]; mqtt_aux(&account, "discovery", account.mac, topic, sizeof(topic));
            announced = publish(topic, mqtt_announcement(&account), true);
        }
        int rc = connected && announced == MQTT_SUCCESS ? MQTTSubscribe(&client, wildcard, account.mode ? QOS1 : QOS0, command) : MQTT_FAILURE;
        if (rc == MQTT_SUCCESS) rc = MQTTSubscribe(&client, "homeassistant/status", QOS0, ha_birth);
        unsigned count = feature_mqtt_filters(feature_filters);
        for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; i++) rc = MQTTSubscribe(&client, feature_filters[i], account.mode ? QOS1 : QOS0, feature_command);
        if (connected && rc != MQTT_SUCCESS) mqtt_status(MQTT_WAITING_ACL);
        if (rc == MQTT_SUCCESS) { cleanup_pending = true; rc = discovery(&cfg); }
        if (rc == MQTT_SUCCESS) rc = states(&cfg, &state);
        if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
        if (rc == MQTT_SUCCESS) rc = publish(availability_topic, "online", true);
        if (rc == MQTT_SUCCESS) { attempt = 0; cleanup_pending = true; mqtt_status(MQTT_READY); }
        uint32_t acl_check = mico_rtos_get_time();
        bool old_on=false;unsigned old_speed=0;
        bool sent = false, old_visible = false; uint32_t revision = 0; rediscover = false;
        while (rc == MQTT_SUCCESS) {
            rc=ota_service(base);if(rc!=MQTT_SUCCESS)break;
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = true; mico_rtos_unlock_mutex(&a1_lock);
            a1_state_get(&state); a1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) break;
            fc=malloc(sizeof(*fc)); if (!fc) break; feature_config_get(fc);
            bool feature_changed=feature_revision!=fc->revision; free(fc); if(feature_changed)break;
            if (rediscover) { feature_mqtt_service(publish,false);rc = discovery(&current); rediscover = false; sent = false; }
            bool on;unsigned speed;feature_effective_fan(current.fan_on,current.speed,&on,&speed);
            if (rc == MQTT_SUCCESS && (!sent || revision != current.revision || old_visible != state.led_visible || old_on!=on || old_speed!=speed)) {
                rc = states(&current, &state); revision = current.revision; old_visible = state.led_visible;old_on=on;old_speed=speed; sent = true;
            }
            if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
            if (rc == MQTT_SUCCESS && account.mode && (uint32_t)(mico_rtos_get_time() - acl_check) >= 30000) {
                rc = MQTTSubscribe(&client, wildcard, QOS1, command);
                for (unsigned i = 0; i < count && rc == MQTT_SUCCESS; ++i)
                    rc = MQTTSubscribe(&client, feature_filters[i], QOS1, feature_command);
                acl_check = mico_rtos_get_time();
                if (rc != MQTT_SUCCESS) mqtt_status(MQTT_WAITING_ACL);
            }
            if (rc == MQTT_SUCCESS) rc = MQTTYield(&client, 100);
            mico_thread_msleep(50);
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
        a1_state_get(&state);
        if (state.mqtt_status == MQTT_READY) mqtt_status(MQTT_DISCONNECTED);
        net.disconnect(&net); MQTTClientDeinit(&client); mico_thread_msleep(mqtt_retry_seconds(attempt++) * 1000);
    }
}
OSStatus a1_mqtt_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_mqtt", mqtt_worker, 12288, 0); }
