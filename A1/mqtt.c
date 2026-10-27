#include "../common/ota/mode.h"
#include "a1.h"
#ifdef A1_FRESH_AIR
#include "fresh_air.h"
#endif
#include "MQTTClient.h"
#include "../common/mqtt_transport.h"
#include "../common/mqtt/client.h"
#include "../common/json_owned.h"
#include "../common/mqtt/service.h"
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
static mico_thread_t mqtt_owner_thread;
static ph_mqtt_startup mqtt_startup;
static mqtt_account_t account;
static char base[192], availability_topic[MQTT_TOPIC_MAX];
static bool rediscover, cleanup_pending;
static uint8_t discovery_language;
static a1_config_t session_config;
static bool same_connection(const a1_config_t *, const a1_config_t *);
static bool session_current(void)
{ a1_config_t c; a1_config_get(&c); return bemfa_primary_enabled() && same_connection(&session_config, &c); }
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
    int result = ph_mqtt_publish(&client, topic, &m);
    if (result == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
        return PH_MQTT_MORE;
    if (result != PH_MQTT_MORE)
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
#include "../common/mqtt/service.h"
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
    a1_control_t r={0};
    if(topic_is(d,"light/set")){
        json_object *o=a1_json_parse(m->payload,m->payloadlen);
        if(o){a1_control_local("light",o);json_object_put(o);}return;
    }
    if(topic_is(d,"fan/set")){r.kind=A1_CONTROL_FAN;r.fields=A1_SET_ON;}
    else if(topic_is(d,"fan/percentage/set")){r.kind=A1_CONTROL_FAN;r.fields=A1_SET_SPEED;}
    else if(topic_is(d,"child_lock/set")){r.kind=A1_CONTROL_LOCK;r.fields=A1_SET_ENABLED;}
    else if(topic_is(d,"led_always_on/set")){r.kind=A1_CONTROL_LIGHT;r.fields=A1_SET_ALWAYS;}
    else return;
    const char *text=m->payload;unsigned n=m->payloadlen,value=0;
    if(r.fields==A1_SET_SPEED){
        unsigned i=0;
        while(i<n && isdigit((unsigned char)text[i])){
            value=value*10+text[i++]-'0';if(value>100)return;
        }
        if(!i)return;
        while(i<n && isspace((unsigned char)text[i]))i++;
        if(i!=n)return;
        r.speed=value;
    }else{
        if(n==2 && !memcmp(text,"ON",2))value=1;
        else if(n!=3 || memcmp(text,"OFF",3))return;
        r.on=r.enabled=r.always_on=value;
    }
    if(service_control_begin()){
        a1_control_apply(&r,false,false);service_control_end();
    }
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
static __attribute__((noinline)) int filter_discovery(const a1_config_t *c, bool clear,unsigned index,service_mqtt_json_scope *scope)
{
    static const char *keys[] = {"filter_remaining", "filter_used", "filter_replacement", "filter_lifetime", "filter_reset", "fan_mode"};
    static const char *components[] = {"sensor", "sensor", "binary_sensor", "number", "button", "select"};
    static const char *names[] = {"滤芯剩余寿命", "滤芯使用时间", "滤芯更换提醒", "滤芯额定寿命", "更换滤芯后重置", "运行模式"};
    static const char *english[] = {"Filter life remaining","Filter usage","Replace filter","Rated filter life","Reset after replacement","Operating mode"};
    bool bound = feature_bound(), grouped = false;
    /* MQTT-owner scratch: PUBACK callbacks only admit copied ingress. */
    static char parent_id[FEATURE_TOPIC_MAX], parent_name[FEATURE_TOPIC_MAX];
    if (!clear && bound && !feature_ha_device(parent_id, sizeof(parent_id),
                                             parent_name, sizeof(parent_name), &grouped))
        return ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_IDENTITY, MQTT_FAILURE);
    for (unsigned i=index; i<6 && i==index; ++i) {
        static char path[MQTT_TOPIC_MAX], identity[MQTT_TOPIC_MAX];
        mqtt_ha(&account, components[i], keys[i], path, sizeof(path));
        /* Wear belongs to this physical A1 even when fan control is grouped. */
        if (clear) { if (publish(path, "", true) != MQTT_SUCCESS) return MQTT_FAILURE; continue; }
        json_object *o = json_object_new_object(), *device = json_object_new_object(), *ids = json_object_new_array();
        if (!o || !device || !ids) {
            if (o) json_object_put(o);
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            return service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        }
        mqtt_identity(&account, "", identity, sizeof(identity));
        json_build_array_add(ids, json_object_new_string(bound ? parent_id : identity));
        json_build_object_add(device, "identifiers", ids);
        str(device, "name", bound ? parent_name : c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_build_object_add(o, "device", device);
        if (bound && grouped) {
            static char label[160];
            snprintf(label, sizeof(label), "%s %s", c->name, ph_i18n(device_language_get(),names[i],english[i]));
            str(o, "name", label);
        } else str(o, "name", ph_i18n(device_language_get(),names[i],english[i]));
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
            const uint8_t language = device_language_get();
            str(o, "value_template", ph_i18n(language,
                "{{ '自动' if value_json.fan_mode == 'auto' else '手动' }}",
                "{{ 'Automatic' if value_json.fan_mode == 'auto' else 'Manual' }}"));
            str(o, "command_template", ph_i18n(language,
                "{\"action\":\"fan.mode.set\",\"params\":{\"mode\":\"{{ 'auto' if value == '自动' else 'manual' }}\"}}",
                "{\"action\":\"fan.mode.set\",\"params\":{\"mode\":\"{{ 'auto' if value == 'Automatic' else 'manual' }}\"}}"));
            json_object *options = json_object_new_array();
            json_build_array_add(options, json_object_new_string(ph_i18n(language, "自动", "Automatic")));
            json_build_array_add(options, json_object_new_string(ph_i18n(language, "手动", "Manual")));
            json_build_object_add(o, "options", options);
        }
        int rc = service_mqtt_optional_send(publish,path,o,true,scope);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}
/* Accessory identity is always the physical A1, even with grouped primary control. */
static int discovery_build(const a1_config_t *,unsigned,service_mqtt_json_scope *);
static int fresh_discovery(const a1_config_t *c,bool clear,unsigned i,service_mqtt_json_scope *scope) {
    char path[MQTT_TOPIC_MAX],identity[MQTT_TOPIC_MAX];
    const char *key=i?"fresh_air_feedback":"fresh_air";
    mqtt_ha(&account,i?"sensor":"fan",key,path,sizeof(path));
#ifndef A1_FRESH_AIR
    clear=true;
    (void)c;(void)scope;
#endif
    if(clear)return publish(path,"",true);
    if(!i)return discovery_build(c,10,scope);
#ifdef A1_FRESH_AIR
    json_object *o=json_object_new_object(),*device=json_object_new_object(),*ids=json_object_new_array();
    if(!o||!device||!ids){if(o)json_object_put(o);if(device)json_object_put(device);if(ids)json_object_put(ids);return service_mqtt_json_error(scope,PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);}
    mqtt_identity(&account,"",identity,sizeof(identity));json_build_array_add(ids,json_object_new_string(identity));
    json_build_object_add(device,"identifiers",ids);str(device,"name",c->name);str(device,"manufacturer","Phicomm");str(device,"model","A1 Air Purifier (EMW3080B)");str(device,"sw_version",A1_VERSION);
    json_build_object_add(o,"device",device);str(o,"name",i?ph_i18n(device_language_get(),"新风转速","Fresh air speed"):ph_i18n(device_language_get(),"新风风机","Fresh air fan"));
    mqtt_identity(&account,key,identity,sizeof(identity));str(o,"unique_id",identity);
    str(o,"availability_topic",availability_topic);topic(o,"state_topic","state");
    str(o,"value_template","{{ value_json.fresh_air_rpm }}");
    str(o,"unit_of_measurement","rpm");str(o,"state_class","measurement");str(o,"entity_category","diagnostic");
    return service_mqtt_optional_send(publish,path,o,true,scope);
#else
    return MQTT_SUCCESS;
#endif
}
static void primary_snapshot(const a1_config_t *c,bool *on,unsigned *speed) {
#ifdef A1_FRESH_AIR
    fresh_snapshot_t f;fresh_snapshot(&f);*on=c->fan_on;*speed=f.primary;
#else
    feature_effective_fan(c->fan_on,c->speed,on,speed);
#endif
}
/* sensor-values-v1 describes effective primary fan percent, not RPM.
 * This owner snapshot also applies to the fresh-air build. */
static uint32_t fan_sensor_boot,fan_sensor_sequence,fan_sensor_at;
static uint32_t fan_sensor_wait=5000;
static bool fan_sensor_started,fan_sensor_turn;
static bool fan_sensor_ready(void) {
    mico_rtos_lock_mutex(&a1_lock);
    bool ready=a1_state.fan_ready && !a1_state.ota_active && !a1_state.reboot_pending;
    mico_rtos_unlock_mutex(&a1_lock);
    return ready;
}
static int fan_sensor_cycle(bool on,unsigned speed,bool *handled) {
    *handled=false;
    if(!account.mode)return MQTT_SUCCESS;
    if(fan_sensor_turn){fan_sensor_turn=false;return MQTT_SUCCESS;}
    uint32_t now=mico_rtos_get_time();
    if(fan_sensor_started && (uint32_t)(now-fan_sensor_at)<fan_sensor_wait)return MQTT_SUCCESS;
    *handled=fan_sensor_turn=fan_sensor_started=true;
    fan_sensor_at=now;fan_sensor_wait=1000;
    if(!fan_sensor_boot && (MicoRandomNumberRead((uint8_t *)&fan_sensor_boot,sizeof(fan_sensor_boot))!=kNoErr || !fan_sensor_boot)) {
        fan_sensor_boot=0;return MQTT_SUCCESS;
    }
    unsigned percent=on?speed:0;
    bool valid=fan_sensor_ready() && percent<=100;
    char topic[192],payload[448],value[8]="null";
    if(valid && !mqtt_format(value,sizeof(value),"%u",percent))return MQTT_SUCCESS;
    uint32_t sequence=fan_sensor_sequence+1u;if(!sequence)sequence=1;
    if(!mqtt_aux(&account,"sensor",account.mac,topic,sizeof(topic)) ||
       !mqtt_format(topic+strlen(topic),sizeof(topic)-strlen(topic),"/state") ||
       !mqtt_format(payload,sizeof(payload),
          "{\"schema\":2,\"model\":\"a1\",\"device_id\":\"%s\",\"boot\":%lu,\"sequence\":%lu,\"sensors\":{\"fan_speed_percent\":{\"type\":\"number\",\"name\":\"Fan speed\",\"value\":%s,\"unit\":\"%%\",\"valid\":%s,\"age_ms\":0}}}",
          account.mac,(unsigned long)fan_sensor_boot,(unsigned long)sequence,value,
          valid?"true":"false"))return MQTT_SUCCESS;
    fan_sensor_sequence=sequence;
    int rc=publish(topic,payload,false);
    if(rc==MQTT_SUCCESS)fan_sensor_wait=5000;
    return rc;
}
static int discovery_build(const a1_config_t *c,unsigned index,service_mqtt_json_scope *scope)
{
    if(index==11)return fresh_discovery(c,false,1,scope);
#ifndef A1_FRESH_AIR
    if(index==10)return fresh_discovery(c,true,0,scope);
#endif
    bool fresh=index==10;
    unsigned begin=fresh?0:index;
    const char *keys[] = {"fan", "light", "child_lock", "led_always_on"};
    const char *components[] = {"fan", "light", "switch", "switch"};
    const char *names[] = {"空气净化器", "灯环", "童锁", "灯环常亮"};
    const char *english[] = {"Air purifier", "Light ring", "Child lock", "Always-on light ring"};
    for (unsigned i=begin; i<4 && i==begin; i++) {
        if (!fresh && feature_bound()) { char path[MQTT_TOPIC_MAX]; mqtt_ha(&account, components[i], fresh?"fresh_air":keys[i], path, sizeof(path)); int rc=publish(path,"",true); if(rc!=MQTT_SUCCESS)return rc; continue; }
        json_object *o = json_object_new_object(), *device = json_object_new_object(), *ids = json_object_new_array();
        if (!o || !device || !ids) {
            if (o) json_object_put(o);
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            return service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        }
        char text[MQTT_TOPIC_MAX], path[MQTT_TOPIC_MAX];
        mqtt_identity(&account, "", text, sizeof(text));
        json_build_array_add(ids, json_object_new_string(text));
        json_build_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "A1 Air Purifier (EMW3080B)");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", A1_VERSION);
        json_build_object_add(o, "device", device); str(o, "name", fresh?ph_i18n(device_language_get(),"新风风机","Fresh air fan"):ph_i18n(device_language_get(),names[i],english[i]));
        mqtt_identity(&account, fresh?"fresh_air":keys[i], text, sizeof(text)); str(o, "unique_id", text);
        str(o, "availability_topic", availability_topic);
        topic(o,"json_attributes_topic","state");
        str(o,"json_attributes_template","{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"config_dirty\": value_json.config_dirty} | tojson }}");
        json_build_object_add(o, "retain", json_object_new_boolean(false));
        topic(o, "command_topic", "command");
        snprintf(text, sizeof(text), "%s/state", keys[i]); topic(o, "state_topic", fresh?"state":text);
        if(fresh)str(o,"state_value_template","{{ 'ON' if value_json.fresh_air_requested_on else 'OFF' }}");
        if (i == 0) {
            str(o, "icon", "mdi:air-purifier");
            topic(o,"preset_mode_command_topic","command");
            topic(o,"preset_mode_state_topic",fresh?"state":"fan/mode/state");
            if(fresh)str(o,"preset_mode_value_template","{{ value_json.fresh_air_mode }}");
            snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{\"mode\":{{ value | tojson }}}}",fresh?"fresh_air.set":"fan.mode.set");str(o,"preset_mode_command_template",text);
            json_object *modes=json_object_new_array();
            json_build_array_add(modes,json_object_new_string("auto"));
            json_build_array_add(modes,json_object_new_string("manual"));
            json_build_object_add(o,"preset_modes",modes);
            topic(o, "percentage_command_topic", "command");
            snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{\"on\":{{ 'true' if value == 'ON' else 'false' }}}}",fresh?"fresh_air.set":"fan.set");str(o,"command_template",text);
            snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{\"mode\":\"manual\",%s\"speed\":{{ value | int }}}}",fresh?"fresh_air.set":"fan.set",fresh?"\"on\":{{ 'true' if value | int > 0 else 'false' }},":"");str(o,"percentage_command_template",text);
            topic(o, "percentage_state_topic", fresh?"state":"fan/percentage/state");
            if(fresh)str(o,"percentage_value_template","{{ value_json.fresh_air_speed }}");
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
        mqtt_ha(&account, components[i], fresh?"fresh_air":keys[i], path, sizeof(path));
        int rc = service_mqtt_optional_send(publish,path,o,true,scope);
        if (rc != MQTT_SUCCESS) return rc;
    }
    if(fresh)return MQTT_SUCCESS;
    return index>=4?filter_discovery(c,false,index-4,scope):MQTT_SUCCESS;
}
static int states(const a1_config_t *c, const mqtt_state_t *s,unsigned index)
{
    bool on; unsigned desired;primary_snapshot(c,&on,&desired);
    int rc=index==0?leaf("fan/state",on?"ON":"OFF"):MQTT_SUCCESS;
    char speed[4], light[192]; snprintf(speed, sizeof(speed), "%u", on ? desired : 0);
    if (index==1 && rc==MQTT_SUCCESS) rc = leaf("fan/percentage/state", speed);
    if (index==2 && rc==MQTT_SUCCESS) rc = leaf("fan/mode/state",feature_fan_auto()?"auto":"manual");
    snprintf(light, sizeof(light), "{\"state\":\"%s\",\"color_mode\":\"rgb\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
        s->led_visible ? "ON" : "OFF", c->brightness, c->red, c->green, c->blue);
    if (index==3 && rc==MQTT_SUCCESS) rc = leaf("light/state", light);
    if (index==4 && rc==MQTT_SUCCESS) rc = leaf("child_lock/state", c->child_lock ? "ON" : "OFF");
    if (index==5 && rc==MQTT_SUCCESS) rc = leaf("led_always_on/state", c->led_always_on ? "ON" : "OFF");
    if(index==6 && rc==MQTT_SUCCESS)rc=service_mqtt_snapshot(publish,&account);
    return rc;
}
static bool discovery_deferred;
static uint32_t discovery_retry_at;
static int discovery(const a1_config_t *c,unsigned index)
{
    /* Resource pressure never retains a graph or retries at owner-loop rate.
     * One fixed index is retried at most once/second; normal state keeps moving. */
    uint32_t now=mico_rtos_get_time();
    if(discovery_deferred && (uint32_t)(now-discovery_retry_at)<1000u)return PH_MQTT_MORE;
    service_mqtt_json_scope scope;
    int rc=PH_MQTT_MORE;
    if(service_mqtt_json_begin(&scope,&mqtt_owner_thread)) {
        rc=discovery_build(c,index,&scope);
        service_mqtt_json_end(&scope);
    }
    discovery_deferred=rc==PH_MQTT_MORE;
    if(discovery_deferred)discovery_retry_at=now;
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
    for(unsigned i=0;i<2;i++)if(fresh_discovery(&session_config,true,i,NULL)!=MQTT_SUCCESS)result=MQTT_FAILURE;
    for(unsigned i=0;i<6;i++)if(filter_discovery(&session_config,true,i,NULL)!=MQTT_SUCCESS)result=MQTT_FAILURE;
    if (feature_mqtt_cleanup(publish) != MQTT_SUCCESS) result = MQTT_FAILURE;
    return result;
}
static bool same_connection(const a1_config_t *a, const a1_config_t *b)
{
    return a->mqtt_port == b->mqtt_port && !strcmp(a->mqtt_host, b->mqtt_host) &&
        !strcmp(a->mqtt_user, b->mqtt_user) && !strcmp(a->mqtt_password, b->mqtt_password) && !strcmp(a->name, b->name);
}
/* The broad virtual-device subscription also echoes our own live telemetry.
 * Admit only leaves with a deferred business consumer; a broker's transport
 * PUBACK must not turn outgoing state into a flood of no-op commands. */
static bool feature_ingress_topic(const char *topic, size_t length)
{
    for (unsigned i = 0; i < 3; ++i)
        if (feature_filters[i][0] && ph_mqtt_topic_matches(feature_filters[i], topic, length))
            return true; /* Link RPC request/response and retained address. */
    size_t filter_length = strlen(feature_filters[3]);
    if (filter_length < 2)
        return false;
    size_t root_length = filter_length - 2; /* Session-owned root + "/#". */
    if (length <= root_length || memcmp(topic, feature_filters[3], root_length) ||
        topic[root_length] != '/')
        return false;
    const char *leaf = topic + root_length + 1;
    size_t leaf_length = length - root_length - 1;
    /* The non-group virtual root shares the physical linkage domain. Its
     * original base/linkage/get and set consumers run before virtual_input. */
    size_t base_length = strlen(base);
    if (root_length == base_length + 8 && !memcmp(topic, base, base_length) &&
        !memcmp(topic + base_length, "/linkage", 8) && leaf_length == 3 &&
        (!memcmp(leaf, "get", 3) || !memcmp(leaf, "set", 3)))
        return true;
    /* Group controls may address a colon-separated member MAC. The deferred
     * parser retains the authoritative membership/authorization checks. */
    if (leaf_length > 18 && leaf[2] == ':' && leaf[5] == ':' &&
        leaf[8] == ':' && leaf[11] == ':' && leaf[14] == ':' && leaf[17] == '/') {
        leaf += 18;
        leaf_length -= 18;
    }
    static const char *const leaves[] = {
        "command", "fan/set", "fan/percentage/set", "light/set", "child_lock/set",
        "led_always_on/set", "binding/get", "binding/set", "binding/state",
        "linkage/get", "linkage/set", "linkage/state", "light/config/get",
        "light/config/set", "light/config/state", "schedule/get", "schedule/set",
        "schedule/state", "sleep/get", "sleep/set", "sleep/state",
        "time/get", "time/set", "time/state"
    };
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); ++i)
        if (strlen(leaves[i]) == leaf_length && !memcmp(leaf, leaves[i], leaf_length))
            return true;
    return false;
}

static bool ingress_accept(MessageData *d,messageHandler handler) {
    if(handler==ha_birth)return true;
    if(handler==feature_command) {
        MQTTString *t = d->topicName;
        const char *topic = t->cstring ? t->cstring : t->lenstring.data;
        size_t length = t->cstring ? strlen(t->cstring) : (size_t)t->lenstring.len;
        return feature_ingress_topic(topic, length);
    }
    if(d->message->retained)return false;
    if(topic_is(d,"command") || topic_is(d,"ota/set") || topic_is(d,"fan/set") ||
       topic_is(d,"fan/percentage/set") || topic_is(d,"light/set") ||
       topic_is(d,"child_lock/set") || topic_is(d,"led_always_on/set"))return true;
    MQTTString *t=d->topicName;const char *p=t->cstring?t->cstring:t->lenstring.data;
    size_t n=t->cstring?strlen(t->cstring):(size_t)t->lenstring.len;
    for(unsigned i=0;i<FEATURE_MQTT_A1_FILTER_MAX;i++)
        if(feature_filters[i][0] && ph_mqtt_topic_matches(feature_filters[i],p,n))return true;
    return false;
}

static a1_config_t cfg,current;
static mqtt_state_t state;
static char client_id[192],wildcard[MQTT_TOPIC_MAX];
static unsigned count,feature_revision;
static bool old_on,sent,old_visible,filter_turn;
static unsigned discovery_index,state_index,state_pending,state_seen;
static unsigned old_speed;
static uint32_t revision,filter_sent_at;
static a1_filter_storage_t sent_filter;
/* A dirty snapshot schedules every leaf. Keep pending work across turns:
 * feature_revision is an observation edge, not the lifetime of a batch. */
static int states_step(const a1_config_t *config, const mqtt_state_t *snapshot, bool dirty, bool *snapshot_sent) {
    *snapshot_sent = false;
    if (dirty)
        state_pending |= 0x7fu;
    for (unsigned pass = 0; pass < 7; pass++) {
        unsigned index = (state_index + pass) % 7;
        if (!(state_pending & (1u << index)))
            continue;
        int rc = states(config, snapshot, index);
        if (rc == MQTT_SUCCESS) {
            *snapshot_sent = index == 6;
            state_pending &= ~(1u << index);
            state_index = (index + 1) % 7;
            state_seen |= 1u << index;
            sent = state_seen == 0x7fu;
        }
        return rc;
    }
    return MQTT_SUCCESS;
}
/* Urgent filter snapshots may lead one turn, but never consume successive
 * turns while other leaves remain pending, even under continuous resets. */
static bool filter_priority(bool due) {
    if (due && (!filter_turn || !(state_pending & 0x3fu))) {
        filter_turn = true;
        return true;
    }
    filter_turn = false;
    return false;
}
static int session_prepare(void *context,ph_mqtt_connection *spec) {
    (void)context;
        ota_job_poll();
        a1_config_get(&cfg); mqtt_state_get(&state);
        if (cleanup_pending && state.wifi_connected && !same_connection(&session_config, &cfg)) {
            int result = mqtt_cleanup_reconnect(&client, &account, session_config.mqtt_host,
                session_config.mqtt_port, session_config.mqtt_user, session_config.mqtt_password, cleanup_all);
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_cleanup_warning = result != MQTT_SUCCESS; mico_rtos_unlock_mutex(&a1_lock);
            cleanup_pending = false;
        }
        if (!bemfa_primary_enabled() || !cfg.mqtt_host[0] || !state.wifi_connected) { mqtt_status(MQTT_DISCONNECTED); return PH_MQTT_PREPARE_WAIT; }
        if (!mqtt_account_init(&account, MQTT_TOPIC_TMCP, "a1", cfg.mqtt_user, a1_id)) {
            mqtt_status(MQTT_CONFIG_ERROR); return PH_MQTT_PREPARE_WAIT;
        }
        strcpy(base, account.base);
        mqtt_identity(&account, "", client_id, sizeof(client_id));
        mqtt_leaf(&account, "command", wildcard, sizeof(wildcard));
        mqtt_leaf(&account, "availability", availability_topic, sizeof(availability_topic));
        session_config = cfg;discovery_language=device_language_get();
        feature_mqtt_session(&account);
        feature_revision=feature_config_revision();
        count=feature_mqtt_filters(feature_filters,sizeof(feature_filters)/sizeof(feature_filters[0]));
        if(count>sizeof(feature_filters)/sizeof(feature_filters[0]))return PH_MQTT_PREPARE_WAIT;
        rediscover=false;
        *spec=(ph_mqtt_connection){&account,cfg.mqtt_host,cfg.mqtt_user,cfg.mqtt_password,
            client_id,availability_topic,cfg.mqtt_port,5000,mqtt_socket_read};
        return PH_MQTT_PREPARE_READY;
}
static bool session_subscription(void *context,unsigned index,ph_mqtt_subscription *out) {
    (void)context;
    if(index==0)*out=(ph_mqtt_subscription){wildcard,QOS1,command};
    else if(index==1)*out=(ph_mqtt_subscription){"homeassistant/status",QOS0,ha_birth};
    else if(index-2<count)*out=(ph_mqtt_subscription){feature_filters[index-2],QOS1,feature_command};
    else return false;
    return true;
}
static int session_initial(void *context) {
    feature_mqtt_session(&account);
    (void)context;
    int rc=service_mqtt_capabilities(publish,&account);
    discovery_deferred=false;discovery_retry_at=0;
    discovery_index=state_index=0;state_pending=0x7fu;state_seen=0;
    old_on=sent=old_visible=filter_turn=false;old_speed=revision=filter_sent_at=0;
    memset(&sent_filter,0,sizeof(sent_filter));
    return rc;
}
static int session_cycle(void *context) {
    (void)context;
            ota_job_poll();
            
            int rc=ota_service(base);if(rc!=MQTT_SUCCESS)return rc;
            if(ota_job_active())return MQTT_SUCCESS;
            mqtt_state_get(&state); a1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) return PH_MQTT_RECONFIGURE;
            uint8_t language=device_language_get();
            if(language!=discovery_language){discovery_language=language;rediscover=true;}
            uint32_t current_feature_revision=feature_config_revision();
            bool feature_changed=feature_revision!=current_feature_revision;
            feature_revision=current_feature_revision;
            if(feature_changed && !feature_mqtt_filters_current(feature_filters,count))return PH_MQTT_RECONFIGURE;
            /* A birth received during a batch requests a complete subsequent pass.
             * Do not consume it by restarting the partially published pass. */
            if(rediscover && discovery_index==12) {feature_mqtt_service(publish,false);discovery_index=0;rediscover=false;sent=false;state_index=0;state_pending=0x7fu;state_seen=0;}
            if(discovery_index<12) {
                rc=discovery(&current,discovery_index);
                if(rc==MQTT_SUCCESS)++discovery_index;
                if(rc!=PH_MQTT_MORE)return rc;
                rc=MQTT_SUCCESS;
            }
            bool on;unsigned speed;primary_snapshot(&current,&on,&speed);
            a1_filter_storage_t filter; a1_filter_get(&filter);
            bool dirty = feature_changed || revision != current.revision ||
                old_visible != state.led_visible || old_on != on || old_speed != speed;
            revision=current.revision;old_visible=state.led_visible;old_on=on;old_speed=speed;
            /* Filter reset/lifetime changes need their own immediate state
             * turn. Waiting behind six unrelated leaves can hide the zero
             * usage edge while the running fan accrues its next second. */
            if (dirty)
                state_pending |= 0x7fu;
#ifdef A1_FRESH_AIR
            static uint32_t fresh_at;
            uint32_t fresh_now=mico_rtos_get_time();
            if((uint32_t)(fresh_now-fresh_at)>=1000){state_pending|=1u<<6;fresh_at=fresh_now;}
#endif
            /* At most one independent typed attempt; the next owner turn
             * always remains available to native/filter/feature producers. */
            bool sensor_handled;
            int sensor_rc=fan_sensor_cycle(on,speed,&sensor_handled);
            if(sensor_handled)return sensor_rc;
            if (filter_priority(a1_filter_mqtt_due(&filter, &sent_filter, mico_rtos_get_time(), filter_sent_at))) {
                rc = service_mqtt_snapshot(publish, &account);
                if (rc == MQTT_SUCCESS) {
                    sent_filter = filter; filter_sent_at = mico_rtos_get_time();
                    state_pending &= ~(1u << 6); state_seen |= 1u << 6;
                    sent = state_seen == 0x7fu;
                }
            } else if (state_pending) {
                bool snapshot_sent;
                rc=states_step(&current,&state,false,&snapshot_sent);
                if (snapshot_sent) { sent_filter = filter; filter_sent_at = mico_rtos_get_time(); }
            }
            /* Pending native work retains its cursor, but cannot suppress the
             * independent feature producer during resource backoff or quota. */
            if (rc == MQTT_SUCCESS || rc == PH_MQTT_MORE) {
                int feature_rc=feature_mqtt_service(publish,true);
                if(feature_rc!=MQTT_SUCCESS)rc=feature_rc;
            }
    return rc;
}
static bool session_valid(void *context) {
    (void)context;
    a1_config_get(&current);
    mqtt_state_get(&state);
    return bemfa_primary_enabled() && state.wifi_connected && same_connection(&cfg, &current);
}
static bool session_initial_ready(void *context) {
    (void)context;
    return discovery_index==12 && sent && feature_mqtt_initial_ready();
}
static void session_closing(void *context,bool connected,int rc) {
    (void)context;
    service_mqtt_clear();
    if(rc<0)mqtt_error("session",rc);
        mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&a1_lock);
        a1_config_get(&current);
        if(!same_connection(&cfg,&current))ota_forget();
        if (connected && !same_connection(&cfg, &current)) {
            int cleanup = cleanup_all();
            cleanup_pending = false;
            mico_rtos_lock_mutex(&a1_lock); a1_state.mqtt_cleanup_warning = cleanup != MQTT_SUCCESS; mico_rtos_unlock_mutex(&a1_lock);
        }
        feature_mqtt_service(publish,false);
}
static void session_event(void *context,unsigned event,int rc) {
    (void)context;
    if(event==PH_MQTT_CONNECTING)mqtt_status(MQTT_CONNECTING);
    else if(event==PH_MQTT_CONNECT_ERROR)mqtt_status(rc==4||rc==5?MQTT_AUTH_FAILED:MQTT_DISCONNECTED);
    else if(event==PH_MQTT_SUBSCRIBE_ERROR)mqtt_status(MQTT_WAITING_ACL);
    else if(event==PH_MQTT_READY){cleanup_pending=true;mqtt_status(MQTT_READY);}
    else {mqtt_state_get(&state);if(state.mqtt_status==MQTT_READY)mqtt_status(MQTT_DISCONNECTED);}
}
static void mqtt_worker(mico_thread_arg_t arg) {
    if(!ph_mqtt_startup_wait(&mqtt_startup,(uint32_t)arg) ||
       !ph_mqtt_owner_init(&account,8192,&mqtt_owner_thread,ingress_accept)) {
        mqtt_status(MQTT_CONFIG_ERROR);mico_rtos_delete_thread(NULL);return;
    }
    const ph_mqtt_session_hooks hooks={NULL,session_prepare,session_subscription,
        session_initial,session_cycle,session_closing,session_event,NULL,session_initial_ready,session_valid};
    ph_mqtt_session_run(&client,&hooks);
}
OSStatus a1_mqtt_start(void)
{
    uint32_t generation=ph_mqtt_startup_prepare(&mqtt_startup);
    OSStatus rc=mico_rtos_create_thread(&mqtt_owner_thread,MICO_APPLICATION_PRIORITY,
        "a1_mqtt",mqtt_worker,16384,generation);
    ph_mqtt_startup_publish(&mqtt_startup,generation,rc==kNoErr);return rc;
}
