/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef PHICOMM_BEMFA
#include "mico.h"
#include "bemfa.h"
#include "bemfa_transport.h"
#include "service.h"
#include "../mqtt_transport.h"
#include "../device_status.h"
#include <stdlib.h>
#include <string.h>
#ifdef BEMFA_TC1
#define CHANNELS 6
#define BEMFA_MODEL "tc1"

#else
#define CHANNELS 2
#define BEMFA_MODEL "a1"

#endif
/* Deferred POD queue: no business action runs inside protocol dispatch. */
#define COMMAND_SLOTS 4u
typedef struct { bemfa_command_t command; uint32_t revision, session; } queued_command;
static queued_command commands[COMMAND_SLOTS];
static unsigned command_head, command_tail, session_generation, report_dirty;
static unsigned state, errors;
static int last_error;
static unsigned last_stage;
typedef struct {
    bemfa_config_t config;
    Client client;
    char reported[CHANNELS][16];
    unsigned cursor;
    uint32_t report_at;
    Network network;
    uint32_t retry_at;
    unsigned attempt;
    bool connected;
} worker_t;
/* Only the HTTP owner accesses this allocation; status copies scalar atomics. */
static worker_t *worker;
void bemfa_status_capture(struct device_network *s) {
    bemfa_config_t c;bemfa_config_get(&c);
    s->mqtt_enabled=c.mqtt_enabled;s->bemfa_enabled=c.enabled;
    s->bemfa_status=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
    s->bemfa_errors=__atomic_load_n(&errors,__ATOMIC_RELAXED);
    s->bemfa_last_error=__atomic_load_n(&last_error,__ATOMIC_RELAXED);
    s->bemfa_last_stage=__atomic_load_n(&last_stage,__ATOMIC_RELAXED);
}
static void set_state(unsigned v) { __atomic_store_n(&state,v,__ATOMIC_RELEASE); }
static void failed(int rc) {
    __atomic_fetch_add(&errors,1,__ATOMIC_RELAXED);
    __atomic_store_n(&last_error,rc,__ATOMIC_RELAXED);
}
static bool bemfa_current_config(void) {
    bemfa_config_t c;bemfa_config_get(&c);
    return c.enabled && c.revision==worker->config.revision && bemfa_network_ready();
}
static bool bemfa_ingress_accept(MessageData *d,messageHandler handler) {
    (void)handler;
    return !d->message->retained && d->message->payloadlen>0 && d->message->payloadlen<=16;
}
static void bemfa_command(MessageData *d) {
    if (!bemfa_current_config()) return;
    unsigned i;
    const char *topic=d->topicName->cstring;
    if (!topic) return; /* queued transport always owns a terminated topic */
    for(i=0;i<CHANNELS;i++)if(!strcmp(topic,worker->config.topics[i]))break;
    if(i==CHANNELS)return;
#ifdef BEMFA_TC1
    unsigned kind=BEMFA_SOCKET;
#else
    unsigned kind=i?BEMFA_LIGHT:BEMFA_FAN;
#endif
    bemfa_command_t parsed;
    if(!bemfa_parse(kind,d->message->payload,d->message->payloadlen,&parsed))return;
    unsigned head=__atomic_load_n(&command_head,__ATOMIC_RELAXED);
    if(head-__atomic_load_n(&command_tail,__ATOMIC_ACQUIRE)>=COMMAND_SLOTS)return;
    parsed.kind=kind;parsed.index=i;
    commands[head%COMMAND_SLOTS]=(queued_command){parsed,worker->config.revision,
        __atomic_load_n(&session_generation,__ATOMIC_ACQUIRE)};
    __atomic_store_n(&command_head,head+1,__ATOMIC_RELEASE);
}
void bemfa_dispatch(void *owner) {
    unsigned tail=__atomic_load_n(&command_tail,__ATOMIC_RELAXED);
    if(tail==__atomic_load_n(&command_head,__ATOMIC_ACQUIRE))return;
    queued_command item=commands[tail%COMMAND_SLOTS];
    bemfa_config_t c;bemfa_config_get(&c);
    if(!c.enabled || c.revision!=item.revision || !bemfa_network_ready() ||
       __atomic_load_n(&state,__ATOMIC_ACQUIRE)!=MQTT_READY ||
       item.session!=__atomic_load_n(&session_generation,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&command_tail,tail+1,__ATOMIC_RELEASE);return;
    }
    service_mqtt_json_scope scope;
    if(!service_mqtt_json_begin(&scope,owner))return;
    __atomic_store_n(&command_tail,tail+1,__ATOMIC_RELEASE);
    bemfa_command_t parsed=item.command;
    unsigned kind=parsed.kind,i=parsed.index;
    json_object *request=json_object_new_object(),*params=json_object_new_object();
    if(request && params) {
        const char *action=kind==BEMFA_SOCKET?"outlet.set":kind==BEMFA_FAN?"fan.set":"light.set";
        json_build_object_add(request,"action",json_object_new_string(action));
        json_build_object_add(params,kind==BEMFA_LIGHT?"enabled":"on",json_object_new_boolean(parsed.on));
        if(kind==BEMFA_SOCKET)json_build_object_add(params,"index",json_object_new_int(i));
        if(parsed.has_value)json_build_object_add(params,kind==BEMFA_FAN?"speed":"brightness",json_object_new_int(parsed.value));
        json_build_object_add(request,"params",params);params=NULL;
        /* No business action is allowed after a partial graph allocation. */
        if(!service_json_scope_failed(scope.token)) {
            json_object *result=service_command(request);
            if(result)json_object_put(result);
        }
    }
    if(params)json_object_put(params);
    if(request)json_object_put(request);
    service_mqtt_json_end(&scope);
    /* Reports are captured from canonical product state, never the request. */
    __atomic_fetch_or(&report_dirty,1u<<i,__ATOMIC_RELEASE);
}
static bool bemfa_capture(device_status_t *s) {
#ifdef BEMFA_TC1
    return tc1_status_capture(s,NULL);
#else
    a1_status_capture(s,NULL);return true;
#endif
}
static int bemfa_report_one(void) {
    unsigned dirty=__atomic_exchange_n(&report_dirty,0,__ATOMIC_ACQ_REL);
    for(unsigned n=0;n<CHANNELS;n++)if(dirty&(1u<<n))worker->reported[n][0]=0;
    if(dirty)worker->report_at=0;
    uint32_t now=mico_rtos_get_time();
    if(worker->report_at && (int32_t)(now-worker->report_at)<0)return MQTT_SUCCESS;
    unsigned i=worker->cursor++;
    if(worker->cursor==CHANNELS){worker->cursor=0;worker->report_at=now+1000;}
    if(!worker->config.topics[i][0])return MQTT_SUCCESS;
    device_status_t s;if(!bemfa_capture(&s))return MQTT_SUCCESS;
    char wire[16];
#ifdef BEMFA_TC1
    if(s.control.tc1.states[i]<0)return MQTT_SUCCESS;
    strcpy(wire,s.control.tc1.states[i]?"on":"off");
#else
    if(i) {
        if(!s.control.a1.led_visible || !s.control.a1.brightness)strcpy(wire,"off");
        else {
            unsigned percent=(s.control.a1.brightness*100u+127u)/255u;
            snprintf(wire,sizeof(wire),"on#%u",percent?percent:1);
        }
    } else {
        if(!s.control.a1.effective_on || !s.control.a1.effective_speed)strcpy(wire,"off");
        else snprintf(wire,sizeof(wire),"on#%u",bemfa_fan_gear(s.control.a1.effective_speed));
    }
#endif
    if(!strcmp(wire,worker->reported[i]))return MQTT_SUCCESS;
    char topic[BEMFA_TOPIC_BYTES+4];snprintf(topic,sizeof(topic),"%s/up",worker->config.topics[i]);
    /* Bemfa updates /up without returning PUBACK. QoS0 avoids reconnecting
     * after a successful state update; controls remain QoS1 subscriptions. */
    MQTTMessage message={0};message.qos=QOS0;message.payload=wire;message.payloadlen=strlen(wire);
    int rc=bm_publish(&worker->client,topic,&message);
    if(rc==MQTT_SUCCESS)strcpy(worker->reported[i],wire);
    else if(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)worker->report_at=0;
    return rc;
}
static void bemfa_close_connection(Network *network) {
    __atomic_fetch_add(&session_generation,1,__ATOMIC_ACQ_REL);
    bm_pending_clear();
    if(worker->client.isconnected)bm_disconnect(&worker->client);
    if(network->disconnect)network->disconnect(network);
    bm_client_deinit(&worker->client);
}
void bemfa_stop(void) {
    if(worker) {
        bemfa_close_connection(&worker->network);bm_transport_release();
        memset(worker,0,sizeof(*worker));free(worker);worker=NULL;
    }
    __atomic_store_n(&command_tail,__atomic_load_n(&command_head,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
    set_state(MQTT_DISCONNECTED);
}
/* The always-running HTTP owner has a verified 12 KiB stack. Polling here
 * saves a second RTOS stack while keeping both sessions/queues independent.
 * Protocol calls finish before JSON/business dispatch starts. */
void bemfa_poll(void *owner) {
    bemfa_config_t c;bemfa_config_get(&c);
    if(worker && (!c.enabled || c.revision!=worker->config.revision || !bemfa_network_ready())) {
        bemfa_stop();
    }
    if(!c.enabled || !bemfa_network_ready())return;
    if(!bemfa_config_valid(&c,CHANNELS==6)){set_state(MQTT_CONFIG_ERROR);return;}
    if(!worker) {
        const micoMemInfo_t *m=MicoGetMemoryInfo();
        if(!m || m->free_memory<(int)(SERVICE_MEMORY_RESERVE+5120))return;
        worker=calloc(1,sizeof(*worker));
        if(!worker || !bm_transport_allocate(owner)) {
            if(worker){free(worker);worker=NULL;}failed(-1);return;
        }
        worker->config=c;worker->network.my_socket=-1;
        if(!bm_owner_init(NULL,16,owner,bemfa_ingress_accept)) {
            bm_transport_release();free(worker);worker=NULL;failed(-2);return;
        }
    }
    uint32_t now=mico_rtos_get_time();
    if(worker->retry_at && (int32_t)(now-worker->retry_at)<0)return;
    int rc=MQTT_SUCCESS;
    unsigned stage=PH_MQTT_ERROR_NETWORK;
    if(!worker->connected) {
        __atomic_fetch_add(&session_generation,1,__ATOMIC_ACQ_REL);
        bm_session_begin();set_state(MQTT_CONNECTING);
        memset(&worker->client,0,sizeof(worker->client));
        memset(worker->reported,0,sizeof(worker->reported));worker->cursor=0;worker->report_at=0;
        memset(&worker->network,0,sizeof(worker->network));worker->network.my_socket=-1;
        ssl_opts options={0};
        rc=NewNetwork(&worker->network,"bemfa.com",9501,options);
        if(rc==MQTT_SUCCESS) {
            stage=PH_MQTT_ERROR_INIT;
            worker->network.mqttread=mqtt_socket_read;
            rc=bm_client_init(&worker->client,&worker->network,1000);
        }
        if(rc==MQTT_SUCCESS) {
            stage=PH_MQTT_ERROR_CONNECT;
            char client_id[48];snprintf(client_id,sizeof(client_id),"phicomm-%s-%s-bemfa",BEMFA_MODEL,bemfa_device_id());
            MQTTPacket_connectData data=MQTTPacket_connectData_initializer;
            data.MQTTVersion=4;data.keepAliveInterval=30;data.cleansession=1;
            data.clientID.cstring=client_id;data.username.cstring=worker->config.app_id;data.password.cstring=worker->config.secret_key;
            rc=bm_connect(&worker->client,&data);
        }
        if(rc==MQTT_SUCCESS)stage=PH_MQTT_ERROR_SUBSCRIBE;
        for(unsigned i=0;rc==MQTT_SUCCESS && i<CHANNELS;i++)
            if(worker->config.topics[i][0])rc=bm_subscribe(&worker->client,worker->config.topics[i],QOS1,bemfa_command);
        if(rc==MQTT_SUCCESS && bemfa_current_config()) {
            worker->connected=true;worker->attempt=0;set_state(MQTT_READY);
        }
    } else {
        stage=PH_MQTT_ERROR_SESSION;
        rc=bm_poll(&worker->client);
        if(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)rc=MQTT_SUCCESS;
        if(rc==MQTT_SUCCESS && bemfa_current_config())rc=bemfa_report_one();
        if(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)rc=MQTT_SUCCESS;
        if(rc==MQTT_SUCCESS)rc=bm_yield(&worker->client,10);
        bm_resource_round_end(rc==MQTT_SUCCESS);
    }
    if(rc!=MQTT_SUCCESS) {
        __atomic_store_n(&last_stage,stage,__ATOMIC_RELAXED);
        bm_error(stage,rc);failed(rc);
        bemfa_close_connection(&worker->network);worker->connected=false;
        set_state(rc==4 || rc==5?MQTT_AUTH_FAILED:MQTT_DISCONNECTED);
        bm_reconnect();
        worker->retry_at=mico_rtos_get_time()+mqtt_retry_seconds(worker->attempt<5?worker->attempt++:5)*1000u;
    }
}
#endif
