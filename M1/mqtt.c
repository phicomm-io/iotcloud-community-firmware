#include "../common/ota/mode.h"
#include "m1.h"
#include "MQTTClient.h"
#include "../common/mqtt_cleanup.h"
#include "../common/ha_json.h"
#include "../common/ha_command.h"
#include "../common/mqtt/service.h"
#include "../common/sensor_mp.h"

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
static uint32_t sensor_boot, sensor_sequence, sensor_at;
static bool sensor_started,sensor_metadata;
static unsigned sensor_mask;
static uint32_t sensor_revision=1;
static bool rediscover, cleanup_pending;
static uint8_t discovery_language;
static m1_config_t session_config;
static bool session_need_availability, session_subscribed;
static bool same_connection(const m1_config_t *, const m1_config_t *);
static bool display_mqtt_needed(void)
{
    m1_display_config_t display;
    m1_display_config_get(&display);
    if(display.configured)for(unsigned i=0;i<M1_DISPLAY_LANES;i++)
        if(display.lanes[i].source==M1_DISPLAY_MQTT)return true;
    return false;
}
static void session_display_reset(void)
{
    session_subscribed=false;
    m1_display_source_mqtt_reset();
}
static bool session_current(void)
{
    m1_config_t c; m1_config_get(&c);
    bool valid=same_connection(&session_config,&c) &&
        session_need_availability==display_mqtt_needed();
    if(!valid)session_display_reset();
    return valid;
}
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

static int publish_bytes(const char *topic,const void *bytes,size_t length,bool retained)
{
    MQTTMessage message={0};message.qos=QOS1;message.retained=retained;
    message.payload=(void *)bytes;message.payloadlen=length;
    int rc=ph_mqtt_publish(&client,topic,&message);
    if(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)return PH_MQTT_MORE;
    if(rc!=PH_MQTT_MORE)mqtt_error("publish",rc);return rc;
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
    char topic[MQTT_TOPIC_MAX]; snprintf(topic, sizeof(topic), "%s/ha/%s", base, leaf);
    return publish(topic, payload, retain);
}
static int publish_typed_sensors(const mqtt_state_t *s, uint32_t now)
{
    /* Owner-only periodic work, including random/encoding/resource failures.
     * Never retry an optional feed on every 10 ms owner turn. */
    if(sensor_started && (uint32_t)(now-sensor_at)<5000u)return MQTT_SUCCESS;
    sensor_started=true;sensor_at=now;
    if(!sensor_boot && (MicoRandomNumberRead((uint8_t *)&sensor_boot,sizeof(sensor_boot))!=kNoErr || !sensor_boot)){
        sensor_boot=0;return MQTT_SUCCESS;
    }
    const m1_sample_t *v=&s->sample;
    uint32_t age=v->received?now-v->updated_ms:0;
    unsigned fresh=m1_sample_fresh(v,now)?m1_sample_fields(v):0;
    struct sensor_field {const char *id,*name,*unit;double value;uint32_t age;bool valid;} all[6]={
        {"temperature","Temperature","°C",v->temperature_deci_c/10.0f,age,(fresh&1)!=0},
        {"humidity","Humidity","%",v->humidity_deci_pct/10.0f,age,(fresh&2)!=0},
        {"formaldehyde","Formaldehyde","mg/m³",v->hcho_ug_m3/1000.0f,age,(fresh&8)!=0},
        {"pm25","PM2.5","µg/m³",v->pm25_ug_m3,age,(fresh&4)!=0}
    };
    /* Leave room for optional hardware without exposing absent sensors. */
    unsigned count=4;
    if(s->scd40.ready || s->scd40.received)all[count++]=(struct sensor_field){
        "co2","Carbon dioxide","ppm",s->scd40.co2_ppm,
        s->scd40.received?now-s->scd40.updated_ms:0,
        s->scd40.ready&&s->scd40.received&&s->scd40.co2_ppm&&(uint32_t)(now-s->scd40.updated_ms)<20000};
    if(s->barometer.ready || s->barometer.received)all[count++]=(struct sensor_field){
        "pressure","Pressure","Pa",s->barometer.pressure_centi_pa/100.0f,
        s->barometer.received?now-s->barometer.updated_ms:0,m1_barometer_fresh(&s->barometer,now)};
    unsigned mask=(s->scd40.ready||s->scd40.received?1u:0u)|(s->barometer.ready||s->barometer.received?2u:0u);
    if(mask!=sensor_mask){sensor_mask=mask;if(!++sensor_revision)sensor_revision=1;sensor_metadata=false;}
    bool metadata=!sensor_metadata;
    char topic[MQTT_TOPIC_MAX];ph_writer w;
    uint32_t sequence=sensor_sequence+1u;if(!sequence)sequence=1;
    if(!mqtt_aux(&account,"sensor",account.mac,topic,sizeof(topic)) ||
       !mqtt_format(topic+strlen(topic),sizeof(topic)-strlen(topic),metadata?"/metadata":"/state") ||
       !phw_init(&w,PH_WIRE_MSGPACK))return MQTT_SUCCESS;
    if(metadata){
        ph_sensor_metadata_begin(&w,account.mac,sensor_boot,sensor_revision);
        for(unsigned i=0;i<count;i++)ph_sensor_descriptor_write(&w,all[i].id,all[i].name,all[i].unit);
    }else{
        ph_sensor_snapshot_begin(&w,account.mac,sensor_boot,sequence,sensor_revision);
        for(unsigned i=0;i<count;i++)ph_sensor_reading_write(&w,all[i].id,all[i].value,all[i].valid,all[i].age);
    }
    ph_sensor_finish(&w);ph_buf *payload=phw_take(&w);
    if(!payload)return MQTT_SUCCESS;
    int rc=publish_bytes(topic,payload->buf,payload->len,metadata);ph_buf_free(payload);
    if(rc==MQTT_SUCCESS){if(metadata){sensor_metadata=true;sensor_started=false;}else sensor_sequence=sequence;}
    return rc==PH_MQTT_MORE?MQTT_SUCCESS:rc;
}

#include "../common/mqtt/service.h"
static bool ota_launch(const char *url) { return m1_ota_download(url); }
static void ota_snapshot(char *state,char *error,unsigned *progress) {
    mico_rtos_lock_mutex(&m1_lock);
    snprintf(state,32,"%s",m1_state.ota_state);snprintf(error,128,"%s",m1_state.ota_error);
    *progress=m1_state.ota_progress;mico_rtos_unlock_mutex(&m1_lock);
}
static int ota_publish(const char *leaf,const void *bytes,size_t length,bool retained) { char topic[MQTT_TOPIC_MAX];mqtt_leaf(&account,leaf,topic,sizeof(topic));return publish_bytes(topic,bytes,length,retained); }
#include <ctype.h>
#include "../common/ota/mqtt_ota.inc"

static void native_command(MessageData *d)
{
    if (!session_current()) return;
    MQTTString *t=d->topicName; char topic[FEATURE_TOPIC_MAX];
    unsigned n=t->cstring?strlen(t->cstring):t->lenstring.len;
    if(n>=sizeof(topic))return;
    memcpy(topic,t->cstring?t->cstring:t->lenstring.data,n);topic[n]=0;
    char cmd[MQTT_TOPIC_MAX];mqtt_leaf(&account,"command",cmd,sizeof(cmd));
    if(!strcmp(topic,cmd)){service_mqtt_input(publish_bytes,&account,d->message->payload,d->message->payloadlen,d->message->retained);return;}
    char ota_topic[MQTT_TOPIC_MAX]; mqtt_leaf(&account,"ota/set",ota_topic,sizeof(ota_topic));
    if(!strcmp(topic,ota_topic)) { ota_command(base,d->message->payload,d->message->payloadlen,d->message->retained);return; }
    if(ota_job_active())return;
    if(session_subscribed && feature_mqtt_input(topic,d->message->payload,d->message->payloadlen,d->message->retained))return;
    if(!strcmp(topic,command_topic) && !d->message->retained)ha_command_input(d->message->payload,d->message->payloadlen);
}
static void ha_birth(MessageData *data)
{
    if (!session_current()) return;
    if (data->message->payloadlen == 6 && !memcmp(data->message->payload, "online", 6))
        rediscover = true;
}

enum { MQTT_BRIGHTNESS_INDEX=5, MQTT_PRESSURE_INDEX, MQTT_PARTICLES_INDEX, MQTT_DISCOVERY_COUNT=MQTT_PARTICLES_INDEX+8 };

static int discovery_build(const m1_config_t *c,unsigned index,ha_json *o)
{
    static const char *keys[] = {"temperature", "humidity", "formaldehyde", "pm25", "co2", "brightness", "pressure", "pm1", "pm10", "gt_0_3", "gt_0_5", "gt_1_0", "gt_2_5", "gt_5_0", "gt_10_0"};
    static const char *names[] = {"温度", "湿度", "甲醛", "PM2.5", "二氧化碳", "屏幕亮度", "气压", "PM1.0", "PM10", ">0.3 µm", ">0.5 µm", ">1.0 µm", ">2.5 µm", ">5.0 µm", ">10 µm"};
    static const char *english[] = {"Temperature", "Humidity", "Formaldehyde", "PM2.5", "Carbon dioxide", "Screen brightness", "Air pressure", "PM1.0", "PM10", ">0.3 µm", ">0.5 µm", ">1.0 µm", ">2.5 µm", ">5.0 µm", ">10 µm"};
    static const char *units[] = {"°C", "%", "mg/m³", "µg/m³", "ppm", NULL, "hPa", "µg/m³", "µg/m³", "#/0.1 L", "#/0.1 L", "#/0.1 L", "#/0.1 L", "#/0.1 L", "#/0.1 L"};
    static const char *classes[] = {"temperature", "humidity", NULL, "pm25", "carbon_dioxide", NULL, "atmospheric_pressure", "pm1", "pm10", NULL, NULL, NULL, NULL, NULL, NULL};
    mico_rtos_lock_mutex(&m1_lock);bool co2_received=m1_state.scd40.received;
    bool pressure_received=m1_state.barometer.received;
    bool particles_supported=m1_state.mcu_sensor_details;
    mico_rtos_unlock_mutex(&m1_lock);
    for (unsigned i=index; i<MQTT_DISCOVERY_COUNT && i==index; i++) {
        if ((i == 4 && !co2_received) || (i == MQTT_PRESSURE_INDEX && !pressure_received)) continue;
        char topic[MQTT_TOPIC_MAX], text[MQTT_TOPIC_MAX];
        if(i>=MQTT_PARTICLES_INDEX && !particles_supported){
            mqtt_ha(&account,"sensor",keys[i],topic,sizeof(topic));
            return publish(topic,"",true);
        }
        ha_json_begin(o,NULL,false);
        ha_json_string(o, "name", ph_i18n(device_language_get(),names[i],english[i]));
        mqtt_leaf(&account,"ha/state",text,sizeof(text));ha_json_string(o,"json_attributes_topic",text);
        ha_json_string(o,"json_attributes_template",i == MQTT_PRESSURE_INDEX ?
            "{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"barometer_valid\": value_json.barometer.valid} | tojson }}" :
            "{{ {\"ip\": value_json.ip, \"firmware\": value_json.version, \"sensor_valid\": value_json.sensor_valid} | tojson }}");
        ha_json_bool(o,"retain",false);
        mqtt_identity(&account, keys[i], text, sizeof(text)); ha_json_string(o, "unique_id", text);
        ha_json_begin(o,"device",false);
        ha_json_begin(o,"identifiers",true);
        mqtt_identity(&account,"",text,sizeof(text));ha_json_string(o,NULL,text);
        ha_json_end(o);
        ha_json_string(o,"name",c->name);ha_json_string(o,"model","M1 EMW3080B");
        ha_json_string(o,"manufacturer","Phicomm");ha_json_string(o,"sw_version",M1_VERSION);
        ha_json_end(o);
        if (i != MQTT_BRIGHTNESS_INDEX) {
            snprintf(text, sizeof(text), "%s/ha/%s", base, i>=MQTT_PARTICLES_INDEX ? "particles" : i == MQTT_PRESSURE_INDEX ? "pressure" : i == 4 ? "co2" : "sensor"); ha_json_string(o, "state_topic", text);
            if(i>=MQTT_PARTICLES_INDEX)snprintf(text,sizeof(text),"{{ value_json.pm.%s.%s if value_json.available else none }}",i<MQTT_PARTICLES_INDEX+2?"atmospheric_ug_m3":"particles_per_0_1_l",keys[i]);
            else snprintf(text, sizeof(text), "{{ value_json.%s }}", keys[i]); ha_json_string(o, "value_template", text);
            ha_json_string(o, "unit_of_measurement", units[i]); ha_json_string(o, "state_class", "measurement");
            if (classes[i]) ha_json_string(o, "device_class", classes[i]);
            ha_json_int(o,"expire_after",i>=MQTT_PARTICLES_INDEX ? 6 : i == MQTT_PRESSURE_INDEX ? M1_BAROMETER_STALE_MS/1000u : i == 4 ? 20 : 15);
            ha_json_begin(o,"availability",true);
            ha_json_begin(o,NULL,false);ha_json_string(o,"topic",availability_topic);ha_json_end(o);
            ha_json_begin(o,NULL,false);
            snprintf(text,sizeof(text),"%s/ha/%s",base,i>=MQTT_PARTICLES_INDEX?"particles":i==MQTT_PRESSURE_INDEX?"pressure_availability":i==4?"co2_availability":"sensor_availability");
            ha_json_string(o,"topic",text);
            if(i>=MQTT_PARTICLES_INDEX)ha_json_string(o,"value_template","{{ 'online' if value_json.available else 'offline' }}");
            ha_json_end(o);ha_json_end(o);
            ha_json_string(o, "availability_mode", "all");
        } else {
            ha_json_string(o, "command_topic", command_topic);
            snprintf(text, sizeof(text), "%s/ha/brightness/state", base); ha_json_string(o, "state_topic", text);
            ha_json_string(o, "availability_topic", availability_topic);
            ha_json_int(o,"min",0);
            ha_json_int(o,"max",100);
            ha_json_int(o,"step",25);
            ha_json_string(o, "mode", "slider");
            ha_json_string(o,"command_template","{\"action\":\"display.set\",\"params\":{\"brightness\":{{ value | int }}}}");
            ha_json_string(o,"value_template","{{ (value | int) * 25 }}");
            ha_json_string(o,"entity_category","config");
        }
        mqtt_ha(&account, i != MQTT_BRIGHTNESS_INDEX ? "sensor" : "number", keys[i], topic, sizeof(topic));
        ha_json_end(o);
        int rc = ha_json_done(o)?publish(topic,o->data,true):PH_MQTT_MORE;
        if (rc != MQTT_SUCCESS) return rc;
    }
    return MQTT_SUCCESS;
}

static int ha_particles(void)
{
    m1_details_t snapshot;m1_details_snapshot(&snapshot);
    char bytes[640];ha_json w;ha_json_init(&w,bytes,sizeof(bytes));ha_json_begin(&w,NULL,false);
    bool valid=snapshot.supported && m1_details_valid(&snapshot,mico_rtos_get_time());
    ha_json_bool(&w,"available",valid);
    if(valid){
        const uint8_t *raw=snapshot.frames[0].raw;
        static const char *const groups[]={"atmospheric_ug_m3","particles_per_0_1_l"};
        static const char *const fields[]={"pm1","pm25","pm10","gt_0_3","gt_0_5","gt_1_0","gt_2_5","gt_5_0","gt_10_0"};
        ha_json_begin(&w,"pm",false);
        for(unsigned g=0;g<2;g++){
            ha_json_begin(&w,groups[g],false);
            for(unsigned i=0;i<(g?6u:3u);i++)ha_json_int(&w,fields[(g?3:0)+i],m1_details_u16(raw+10+g*6+i*2));
            ha_json_end(&w);
        }
        ha_json_end(&w);
    }
    ha_json_end(&w);return ha_json_done(&w)?publish_leaf("particles",bytes,false):PH_MQTT_MORE;
}
static int ha_state(void)
{
    char bytes[256],ip[16];bool valid,pressure;
    mico_rtos_lock_mutex(&m1_lock);memcpy(ip,m1_state.ip,sizeof(ip));
    valid=m1_sample_fresh(&m1_state.sample,mico_rtos_get_time());pressure=m1_barometer_fresh(&m1_state.barometer,mico_rtos_get_time());mico_rtos_unlock_mutex(&m1_lock);
    ha_json w;ha_json_init(&w,bytes,sizeof(bytes));ha_json_begin(&w,NULL,false);
    ha_json_string(&w,"ip",ip);ha_json_string(&w,"version",M1_VERSION);ha_json_bool(&w,"sensor_valid",valid);
    ha_json_begin(&w,"barometer",false);ha_json_bool(&w,"valid",pressure);ha_json_end(&w);ha_json_end(&w);
    return ha_json_done(&w)?publish_leaf("state",bytes,true):PH_MQTT_MORE;
}
static bool discovery_deferred;
static uint32_t discovery_retry_at;
static int discovery(const m1_config_t *c,unsigned index)
{
    /* Resource pressure never retains a graph or retries at owner-loop rate.
     * One fixed index is retried at most once/second; normal state keeps moving. */
    uint32_t now=mico_rtos_get_time();
    if(discovery_deferred && (uint32_t)(now-discovery_retry_at)<1000u)return PH_MQTT_MORE;
    /* Single bounded output allocation; no JSON graph or allocator lease. */
    char *bytes=malloc(3072);
    int rc=PH_MQTT_MORE;
    if(bytes){ha_json writer;ha_json_init(&writer,bytes,3072);rc=discovery_build(c,index,&writer);free(bytes);}
    discovery_deferred=rc==PH_MQTT_MORE;
    if(discovery_deferred)discovery_retry_at=now;
    return rc;
}
static int cleanup_discovery(void)
{
    const char *keys[] = {"temperature","humidity","formaldehyde","pm25","co2","brightness","pressure","pm1","pm10","gt_0_3","gt_0_5","gt_1_0","gt_2_5","gt_5_0","gt_10_0"};
    const char *components[] = {"sensor","sensor","sensor","sensor","sensor","number","sensor","sensor","sensor","sensor","sensor","sensor","sensor","sensor","sensor"};
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
static char co2_filter[FEATURE_TOPIC_MAX];
static char source_availability_filter[FEATURE_TOPIC_MAX];
static unsigned discovery_index,initial_index;
static uint32_t connect_sequence,co2_sequence,pressure_sequence,sequence,snapshot_at;
static bool co2_discovered,pressure_discovered,particles_discovered;
static uint32_t particles_at;
static int old_co2_fresh,old_pressure_fresh,old_brightness,old_fresh;
static int session_prepare(void *context,ph_mqtt_connection *spec) {
    (void)context;
        session_display_reset();
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
        mqtt_leaf(&account, "ha/command", command_topic, sizeof(command_topic));
        session_config = cfg;discovery_language=device_language_get();
        session_need_availability=display_mqtt_needed();
        feature_mqtt_session(&account);
        /* This subscription depends only on account identity. Do not allocate
         * the full feature configuration merely to form its fixed topic. */
        char sensor_leaf[32];snprintf(sensor_leaf,sizeof(sensor_leaf),"%s/command",account.mac);
        if(!mqtt_aux(&account,"sensor",sensor_leaf,sensor_filter,sizeof(sensor_filter)))return PH_MQTT_PREPARE_WAIT;
        if(!mqtt_aux(&account,"sensor","+/+",co2_filter,sizeof(co2_filter)))return PH_MQTT_PREPARE_WAIT;
        if(session_need_availability && !mqtt_format(source_availability_filter,sizeof(source_availability_filter),"phicomm/v2/%s/+/+/availability",account.key))return PH_MQTT_PREPARE_WAIT;
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
    else if(index==3)*out=(ph_mqtt_subscription){co2_filter,QOS1,native_command};
    /* The original four filters stay usable with existing broker ACLs. A
     * configured MQTT lane needs offline delivery even while displaying the
     * original values, because a button may select that lane at any time. */
    else if(index==4 && session_need_availability)*out=(ph_mqtt_subscription){source_availability_filter,QOS1,native_command};
    else return false;
    return true;
}
static int session_initial(void *context) {
    sensor_metadata=false;sensor_started=false;
    if(!session_current())return PH_MQTT_RECONFIGURE;
    feature_mqtt_session(&account);
    /* The shared owner calls initial only after every planned SUBACK succeeds.
     * Never admit a generic measurement from a partial/failed subscription. */
    session_subscribed=true;
    (void)context;
    int rc=service_mqtt_capabilities(publish_bytes,&account);
    if(rc==MQTT_SUCCESS)rc=publish_leaf("sensor_availability","offline",true);
    discovery_deferred=false;discovery_retry_at=0;
    discovery_index=initial_index=0;
    connect_sequence=state.sample.sequence;co2_sequence=pressure_sequence=sequence=0;
    co2_discovered=state.scd40.received;
    pressure_discovered=state.barometer.received;
    particles_at=mico_rtos_get_time()-3000u;
    mico_rtos_lock_mutex(&m1_lock);particles_discovered=m1_state.mcu_sensor_details;mico_rtos_unlock_mutex(&m1_lock);
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
                rc=service_mqtt_snapshot(publish_bytes,&account);
                if(rc==MQTT_SUCCESS)rc=ha_state();
                if(rc!=MQTT_SUCCESS && rc!=PH_MQTT_MORE)return rc;
                rc=MQTT_SUCCESS;
            }
            rc=ota_service(base); if(rc!=MQTT_SUCCESS)return rc;
            if(ota_job_active())return MQTT_SUCCESS;
            mqtt_state_get(&state); m1_config_get(&current);
            if (!state.wifi_connected || !same_connection(&cfg, &current)) return PH_MQTT_RECONFIGURE;
            rc=publish_typed_sensors(&state,mico_rtos_get_time());
            if(rc!=MQTT_SUCCESS)return rc;
            uint8_t language=device_language_get();
            if(language!=discovery_language){discovery_language=language;rediscover=true;}
            if (state.scd40.received && !co2_discovered) { rediscover = true; co2_discovered = true; }
            mico_rtos_lock_mutex(&m1_lock);bool particle_support=m1_state.mcu_sensor_details;mico_rtos_unlock_mutex(&m1_lock);
            if(particle_support!=particles_discovered){rediscover=true;particles_discovered=particle_support;}
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
            if(rc==MQTT_SUCCESS && (uint32_t)(mico_rtos_get_time()-particles_at)>=3000u){
                particles_at=mico_rtos_get_time();rc=ha_particles();
                if(rc==PH_MQTT_MORE)rc=MQTT_SUCCESS;
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
    bool valid=state.wifi_connected && same_connection(&cfg,&current) &&
        session_need_availability==display_mqtt_needed();
    if(!valid)session_display_reset();
    return valid;
}
static bool session_initial_ready(void *context) {
    (void)context;
    return discovery_index==MQTT_DISCOVERY_COUNT && initial_index==2 && feature_mqtt_initial_ready();
}
static void session_closing(void *context,bool connected,int rc) {
    (void)context;
    session_display_reset();
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
    else if(event==PH_MQTT_SUBSCRIBE_ERROR){session_display_reset();mqtt_status(MQTT_WAITING_ACL);}
    else if(event==PH_MQTT_READY){cleanup_pending=true;mqtt_status(MQTT_READY);}
    else {mqtt_state_get(&state);if(state.mqtt_status==MQTT_READY)mqtt_status(MQTT_DISCONNECTED);}
}
static void mqtt_worker(mico_thread_arg_t arg) {
    if(!ph_mqtt_startup_wait(&mqtt_startup,(uint32_t)arg)) {
        mqtt_status(MQTT_CONFIG_ERROR);mico_rtos_delete_thread(NULL);return;
    }
    /* The network worker has already recorded a successful thread start.
     * Keep this sole owner alive if RTOS mutex allocation is temporarily
     * unavailable; exiting here would disable MQTT until the next reboot. */
    while(!ph_mqtt_owner_init(&account,8192,&mqtt_owner_thread,ingress_accept)) {
        mqtt_status(MQTT_DISCONNECTED);
        custom_log("M1", "MQTT owner initialization deferred");
        mico_thread_msleep(1000);
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
