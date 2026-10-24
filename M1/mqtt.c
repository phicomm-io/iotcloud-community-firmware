#include "../common/ota/mode.h"
#include "m1.h"
#include "MQTTClient.h"
#include "../common/mqtt_cleanup.h"
#include "../common/json_owned.h"
#include "../common/mqtt/service.h"

int m1_mqtt_read(Network *, unsigned char *, int, int);

typedef struct {
    m1_sample_t sample;
    barometer_state_t barometer;
    struct { uint32_t updated_ms,samples; uint16_t co2_ppm; bool ready,received; } scd40;
    bool wifi_connected;
    uint8_t brightness_requested,mqtt_status;
} mqtt_state_t;
static void mqtt_state_get(mqtt_state_t *out)
{
    mico_rtos_lock_mutex(&m1_lock);
    out->sample=m1_state.sample;
    out->barometer=m1_state.barometer;
    out->scd40.updated_ms=m1_state.scd40.updated_ms;out->scd40.samples=m1_state.scd40.samples;
    out->scd40.co2_ppm=m1_state.scd40.co2_ppm;out->scd40.ready=m1_state.scd40.ready;
    out->scd40.received=m1_state.scd40.received;
    out->wifi_connected=m1_state.wifi_connected;out->brightness_requested=m1_state.brightness_requested;
    out->mqtt_status=m1_state.mqtt_status;
    mico_rtos_unlock_mutex(&m1_lock);
}

static Client client;
static mico_thread_t mqtt_owner_thread;
static ph_mqtt_startup mqtt_startup;
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
    if (!payload) { mqtt_error("serialize", MQTT_FAILURE); return MQTT_FAILURE; }
    MQTTMessage message = {0};
    message.qos = QOS1; message.retained = retain;
    message.payload = (void *)payload; message.payloadlen = strlen(payload);
    int result = ph_mqtt_publish(&client, topic, &message);
    if (result == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
        return PH_MQTT_MORE;
    if (result != PH_MQTT_MORE)
        mqtt_error(topic, result);
    return result;
}

static int publish_leaf(const char *leaf, const char *payload, bool retain)
{
    char topic[MQTT_TOPIC_MAX]; snprintf(topic, sizeof(topic), "%s/%s", base, leaf);
    return publish(topic, payload, retain);
}

#include "../common/mqtt/service.h"
static bool ota_launch(const char *url) { return m1_ota_download(url); }
static void ota_snapshot(char *state,char *error,unsigned *progress) {
    mico_rtos_lock_mutex(&m1_lock);
    snprintf(state,32,"%s",m1_state.ota_state);snprintf(error,128,"%s",m1_state.ota_error);
    *progress=m1_state.ota_progress;mico_rtos_unlock_mutex(&m1_lock);
}
static int ota_publish(const char *leaf,const char *text,bool retained) { return publish_leaf(leaf,text,retained); }
#include <ctype.h>
#include "../common/ota/mqtt_ota.inc"
static void brightness_command(MessageData *data)
{
    if(ota_job_active())return;
    MQTTMessage *m = data->message;
    /* Controls are never retained; ignore replayed commands from old clients. */
    if (m->retained || m->payloadlen != 1) return;
    char c = *(char *)m->payload;
    if (c >= '0' && c <= '4') m1_set_brightness(c - '0');
}

static void native_command(MessageData *d)
{
    if (!session_current()) return;
    MQTTString *t=d->topicName; char topic[FEATURE_TOPIC_MAX];
    unsigned n=t->cstring?strlen(t->cstring):t->lenstring.len;
    if(n>=sizeof(topic))return;
    memcpy(topic,t->cstring?t->cstring:t->lenstring.data,n);topic[n]=0;
    char cmd[MQTT_TOPIC_MAX];mqtt_leaf(&account,"command",cmd,sizeof(cmd));
    if(!strcmp(topic,cmd)){service_mqtt_input(publish,&account,d->message->payload,d->message->payloadlen,d->message->retained);return;}
    char ota_topic[MQTT_TOPIC_MAX]; mqtt_leaf(&account,"ota/set",ota_topic,sizeof(ota_topic));
    if(!strcmp(topic,ota_topic)) { ota_command(base,d->message->payload,d->message->payloadlen,d->message->retained);return; }
    if(ota_job_active())return;
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
{ json_build_object_add(o, key, json_object_new_string(value)); }

enum { MQTT_BRIGHTNESS_INDEX=5, MQTT_PRESSURE_INDEX, MQTT_DISCOVERY_COUNT };

static int discovery_build(const m1_config_t *c,unsigned index,service_mqtt_json_scope *scope)
{
    static const char *keys[] = {"temperature", "humidity", "formaldehyde", "pm25", "co2", "brightness", "pressure"};
    static const char *names[] = {"温度", "湿度", "甲醛", "PM2.5", "二氧化碳", "屏幕亮度", "气压"};
    static const char *units[] = {"°C", "%", "mg/m³", "µg/m³", "ppm", NULL, "hPa"};
    static const char *classes[] = {"temperature", "humidity", NULL, "pm25", "carbon_dioxide", NULL, "atmospheric_pressure"};
    mico_rtos_lock_mutex(&m1_lock);bool co2_received=m1_state.scd40.received;
    bool pressure_received=m1_state.barometer.received;
    mico_rtos_unlock_mutex(&m1_lock);
    for (unsigned i=index; i<MQTT_DISCOVERY_COUNT && i==index; i++) {
        if ((i == 4 && !co2_received) || (i == MQTT_PRESSURE_INDEX && !pressure_received)) continue;
        char topic[MQTT_TOPIC_MAX], text[MQTT_TOPIC_MAX];
        json_object *o = json_object_new_object();
        if (!o) return service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        str(o, "name", names[i]);
        mqtt_leaf(&account,"state",text,sizeof(text));str(o,"json_attributes_topic",text);
        str(o,"json_attributes_template",i == MQTT_PRESSURE_INDEX ?
            "{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"barometer_valid\": value_json.barometer.valid} | tojson }}" :
            "{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"sensor_valid\": value_json.sensor_valid} | tojson }}");
        json_build_object_add(o, "retain", json_object_new_boolean(false));
        mqtt_identity(&account, keys[i], text, sizeof(text)); str(o, "unique_id", text);
        json_object *device = json_object_new_object(), *ids = json_object_new_array();
        if (!device || !ids) {
            if (device) json_object_put(device);
            if (ids) json_object_put(ids);
            json_object_put(o); return service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
        }
        mqtt_identity(&account, "", text, sizeof(text));
        json_build_array_add(ids, json_object_new_string(text));
        json_build_object_add(device, "identifiers", ids);
        str(device, "name", c->name); str(device, "model", "M1 EMW3080B");
        str(device, "manufacturer", "Phicomm"); str(device, "sw_version", M1_VERSION);
        json_build_object_add(o, "device", device);
        if (i != MQTT_BRIGHTNESS_INDEX) {
            snprintf(text, sizeof(text), "%s/%s", base, i == MQTT_PRESSURE_INDEX ? "pressure" : i == 4 ? "co2" : "sensor"); str(o, "state_topic", text);
            snprintf(text, sizeof(text), "{{ value_json.%s }}", keys[i]); str(o, "value_template", text);
            str(o, "unit_of_measurement", units[i]); str(o, "state_class", "measurement");
            if (classes[i]) str(o, "device_class", classes[i]);
            json_build_object_add(o, "expire_after", json_object_new_int(i == MQTT_PRESSURE_INDEX ? M1_BAROMETER_STALE_MS/1000u : i == 4 ? 20 : 15));
            json_object *availability = json_object_new_array();
            json_object *node = json_object_new_object(), *sensor = json_object_new_object();
            if (!availability || !node || !sensor) {
                if (availability) json_object_put(availability);
                if (node) json_object_put(node);
                if (sensor) json_object_put(sensor);
                json_object_put(o); return service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
            }
            str(node, "topic", availability_topic);
            snprintf(text, sizeof(text), "%s/%s", base, i == MQTT_PRESSURE_INDEX ? "pressure_availability" : i == 4 ? "co2_availability" : "sensor_availability"); str(sensor, "topic", text);
            json_build_array_add(availability, node); json_build_array_add(availability, sensor);
            json_build_object_add(o, "availability", availability);
            str(o, "availability_mode", "all");
        } else {
            str(o, "command_topic", command_topic);
            snprintf(text, sizeof(text), "%s/brightness/state", base); str(o, "state_topic", text);
            str(o, "availability_topic", availability_topic);
            json_build_object_add(o, "min", json_object_new_int(0));
            json_build_object_add(o, "max", json_object_new_int(100));
            json_build_object_add(o, "step", json_object_new_int(25));
            str(o, "mode", "slider");
            str(o,"command_template","{\"action\":\"display.set\",\"params\":{\"brightness\":{{ value | int }}}}");
            str(o,"value_template","{{ (value | int) * 25 }}");
            str(o,"entity_category","config");
        }
        mqtt_ha(&account, i != MQTT_BRIGHTNESS_INDEX ? "sensor" : "number", keys[i], topic, sizeof(topic));
        int rc = service_mqtt_optional_send(publish,topic,o,true,scope);
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}

static bool discovery_deferred;
static uint32_t discovery_retry_at;
static int discovery(const m1_config_t *c,unsigned index)
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
    const char *keys[] = {"temperature","humidity","formaldehyde","pm25","co2","brightness","pressure"};
    const char *components[] = {"sensor","sensor","sensor","sensor","sensor","number","sensor"};
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
        !strcmp(a->name, b->name);
}

static bool ingress_accept(MessageData *d,messageHandler handler) {
    return handler==ha_birth || !d->message->retained;
}

static m1_config_t cfg,current;
static mqtt_state_t state;
static char client_id[192],wildcard[MQTT_TOPIC_MAX];
static char sensor_filter[FEATURE_TOPIC_MAX];
static unsigned discovery_index,initial_index;
static uint32_t connect_sequence,co2_sequence,pressure_sequence,sequence,snapshot_at;
static bool co2_discovered,pressure_discovered;
static int old_co2_fresh,old_pressure_fresh,old_brightness,old_fresh;
static int session_prepare(void *context,ph_mqtt_connection *spec) {
    (void)context;
        ota_job_poll();
        m1_config_get(&cfg); mqtt_state_get(&state);
        if (cleanup_pending && state.wifi_connected && !same_connection(&session_config, &cfg)) {
            int result = mqtt_cleanup_reconnect(&client, &account, session_config.mqtt_host,
                session_config.mqtt_port, session_config.mqtt_user, session_config.mqtt_password, cleanup_all);
            mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_cleanup_warning = result != MQTT_SUCCESS; mico_rtos_unlock_mutex(&m1_lock);
            cleanup_pending = false;
        }
        if (!cfg.mqtt_host[0] || !state.wifi_connected) { mqtt_status(MQTT_DISCONNECTED); return PH_MQTT_PREPARE_WAIT; }
        if (!mqtt_account_init(&account, MQTT_TOPIC_TMCP, "m1", cfg.mqtt_user, m1_id)) {
            mqtt_status(MQTT_CONFIG_ERROR); return PH_MQTT_PREPARE_WAIT;
        }
        strcpy(base, account.base);
        mqtt_identity(&account, "", client_id, sizeof(client_id));
        mqtt_leaf(&account, "command", wildcard, sizeof(wildcard));
        mqtt_leaf(&account, "availability", availability_topic, sizeof(availability_topic));
        mqtt_leaf(&account, "command", command_topic, sizeof(command_topic));
        session_config = cfg;
        feature_mqtt_session(&account);
        /* This subscription depends only on account identity. Do not allocate
         * the full feature configuration merely to form its fixed topic. */
        char sensor_leaf[32];snprintf(sensor_leaf,sizeof(sensor_leaf),"%s/command",account.mac);
        if(!mqtt_aux(&account,"sensor",sensor_leaf,sensor_filter,sizeof(sensor_filter)))return PH_MQTT_PREPARE_WAIT;
        rediscover=false;
        *spec=(ph_mqtt_connection){&account,cfg.mqtt_host,cfg.mqtt_user,cfg.mqtt_password,
            client_id,availability_topic,cfg.mqtt_port,5000,m1_mqtt_read};
        return PH_MQTT_PREPARE_READY;
}
static bool session_subscription(void *context,unsigned index,ph_mqtt_subscription *out) {
    (void)context;
    if(index==0)*out=(ph_mqtt_subscription){wildcard,QOS1,native_command};
    else if(index==1)*out=(ph_mqtt_subscription){"homeassistant/status",QOS0,ha_birth};
    else if(index==2)*out=(ph_mqtt_subscription){sensor_filter,QOS1,native_command};
    else return false;
    return true;
}
static int session_initial(void *context) {
    feature_mqtt_session(&account);
    (void)context;
    int rc=service_mqtt_capabilities(publish,&account);
    if(rc==MQTT_SUCCESS)rc=publish_leaf("sensor_availability","offline",true);
    discovery_deferred=false;discovery_retry_at=0;
    discovery_index=initial_index=0;
    connect_sequence=state.sample.sequence;co2_sequence=pressure_sequence=sequence=0;
    co2_discovered=state.scd40.received;
    pressure_discovered=state.barometer.received;
    old_pressure_fresh=old_co2_fresh=old_brightness=old_fresh=-1;
    snapshot_at=mico_rtos_get_time()-5000u;
    return rc;
}
static int session_cycle(void *context) {
    (void)context;
            ota_job_poll();
            int rc=MQTT_SUCCESS;
            if(!ota_job_active() && (uint32_t)(mico_rtos_get_time()-snapshot_at)>=5000) {
                /* Resource pressure on the optional full snapshot must not
                 * suppress independent bounded sensor/feature publication.
                 * Retry on the existing period, not every owner turn. */
                snapshot_at=mico_rtos_get_time();
                rc=service_mqtt_snapshot(publish,&account);
                if(rc!=MQTT_SUCCESS && rc!=PH_MQTT_MORE)return rc;
                rc=MQTT_SUCCESS;
            }
            rc=ota_service(base); if(rc!=MQTT_SUCCESS)return rc;
            if(ota_job_active())return MQTT_SUCCESS;
            mqtt_state_get(&state); m1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) return PH_MQTT_RECONFIGURE;
            if (state.scd40.received && !co2_discovered) { rediscover = true; co2_discovered = true; }
            if (state.barometer.received && !pressure_discovered) { rediscover = true; pressure_discovered = true; }
            /* A birth received during a batch requests a complete subsequent pass.
             * Do not consume it by restarting the partially published pass. */
            if (rediscover && discovery_index==MQTT_DISCOVERY_COUNT) {
                feature_mqtt_service(publish,false);
                discovery_index=0;rediscover=false;
                old_brightness = old_fresh = old_co2_fresh = old_pressure_fresh = -1; sequence = co2_sequence = pressure_sequence = 0;
            }
            if(discovery_index<MQTT_DISCOVERY_COUNT) {
                rc=discovery(&current,discovery_index);
                if(rc==MQTT_SUCCESS)++discovery_index;
                if(rc!=PH_MQTT_MORE)return rc;
                rc=MQTT_SUCCESS;
            }
            if(initial_index<2) {
                char value[4];snprintf(value,sizeof(value),"%u",state.brightness_requested);
                rc=initial_index?publish_leaf("sensor","",true):publish_leaf("brightness/state",value,true);
                if(rc==MQTT_SUCCESS)++initial_index;
                return rc;
            }
            bool fresh = m1_sample_fresh(&state.sample, mico_rtos_get_time()) &&
                (state.sample.sequence != connect_sequence);
            if (rc == MQTT_SUCCESS && fresh != old_fresh) {
                rc = publish_leaf("sensor_availability", fresh ? "online" : "offline", true);
                if (rc == MQTT_SUCCESS && !fresh) rc = publish_leaf("sensor", "", true);
                if(rc==MQTT_SUCCESS)old_fresh=fresh;
            }
            if (rc == MQTT_SUCCESS && fresh && sequence != state.sample.sequence) {
                const m1_sample_t *s = &state.sample;
                char payload[192];
                snprintf(payload, sizeof(payload),
                    "{\"temperature\":%s%ld.%ld,\"humidity\":%s%ld.%ld,\"formaldehyde\":%u.%03u,\"pm25\":%u}",
                    s->temperature_deci_c < 0 ? "-" : "", labs((long)s->temperature_deci_c) / 10, labs((long)s->temperature_deci_c) % 10,
                    s->humidity_deci_pct < 0 ? "-" : "", labs((long)s->humidity_deci_pct) / 10, labs((long)s->humidity_deci_pct) % 10,
                    s->hcho_ug_m3 / 1000, s->hcho_ug_m3 % 1000, s->pm25_ug_m3);
                rc = publish_leaf("sensor", payload, true);
                if(rc==MQTT_SUCCESS)sequence=state.sample.sequence;
            }
            bool co2_fresh = state.scd40.ready && state.scd40.received &&
                (uint32_t)(mico_rtos_get_time() - state.scd40.updated_ms) < 20000;
            if (rc == MQTT_SUCCESS && co2_fresh && co2_sequence != state.scd40.samples) {
                char value[32]; snprintf(value, sizeof(value), "{\"co2\":%u}", state.scd40.co2_ppm);
                /* Do not replay an old measurement as fresh after HA restarts. */
                rc = publish_leaf("co2", value, false);
                if (rc == MQTT_SUCCESS) co2_sequence = state.scd40.samples;
            }
            if (rc == MQTT_SUCCESS && co2_fresh != old_co2_fresh) {
                rc = publish_leaf("co2_availability", co2_fresh ? "online" : "offline", true);
                if (rc == MQTT_SUCCESS) old_co2_fresh = co2_fresh;
            }
            bool pressure_fresh = m1_barometer_fresh(&state.barometer,mico_rtos_get_time());
            if (rc == MQTT_SUCCESS && pressure_fresh && pressure_sequence != state.barometer.samples) {
                char value[48];
                /* Driver units are centi-Pa; 10000 centi-Pa equal one hPa. */
                unsigned long pressure=state.barometer.pressure_centi_pa;
                snprintf(value,sizeof(value),"{\"pressure\":%lu.%04lu}",pressure/10000u,pressure%10000u);
                rc = publish_leaf("pressure",value,false);
                if (rc == MQTT_SUCCESS) pressure_sequence=state.barometer.samples;
            }
            if (rc == MQTT_SUCCESS && pressure_fresh != old_pressure_fresh) {
                rc = publish_leaf("pressure_availability",pressure_fresh ? "online" : "offline",true);
                if (rc == MQTT_SUCCESS) old_pressure_fresh=pressure_fresh;
            }
            if (rc == MQTT_SUCCESS && old_brightness != state.brightness_requested) {
                char value[4]; snprintf(value, sizeof(value), "%u", state.brightness_requested);
                rc = publish_leaf("brightness/state", value, true);
                if(rc==MQTT_SUCCESS)old_brightness=state.brightness_requested;
            }
            if (rc == MQTT_SUCCESS) rc = feature_mqtt_service(publish, true);
    return rc;
}
static bool session_valid(void *context) {
    (void)context;
    m1_config_get(&current);
    mqtt_state_get(&state);
    return state.wifi_connected && same_connection(&cfg, &current);
}
static bool session_initial_ready(void *context) {
    (void)context;
    return discovery_index==MQTT_DISCOVERY_COUNT && initial_index==2 && feature_mqtt_initial_ready();
}
static void session_closing(void *context,bool connected,int rc) {
    (void)context;
    service_mqtt_clear();
    if(rc<0)mqtt_error("session",rc);
        mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_connected = false; mico_rtos_unlock_mutex(&m1_lock);
        m1_config_get(&current);
        if(!same_connection(&cfg,&current))ota_forget();
        if (connected && !same_connection(&cfg, &current)) {
            int cleanup = cleanup_all();
            cleanup_pending = false;
            mico_rtos_lock_mutex(&m1_lock); m1_state.mqtt_cleanup_warning = cleanup != MQTT_SUCCESS; mico_rtos_unlock_mutex(&m1_lock);
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
OSStatus m1_mqtt_start(void)
{
    uint32_t generation=ph_mqtt_startup_prepare(&mqtt_startup);
    OSStatus rc=mico_rtos_create_thread(&mqtt_owner_thread,MICO_APPLICATION_PRIORITY,
        "m1_mqtt",mqtt_worker,16384,generation);
    ph_mqtt_startup_publish(&mqtt_startup,generation,rc==kNoErr);return rc;
}
