#include "../../common/i18n.h"
#include "../../common/ota/mode.h"
#include "ota_server/signed_adapter.h"
#include "ota_server/ota_server.h"
#include <ctype.h>
#include "../../common/mqtt_cleanup.h"
#include "../../common/mqtt/service.h"
#include "../../common/a1_decimal.h"
#include "../../common/ha_json.h"
#include "../../common/sensor_mp.h"
#include "../../common/ha_command.h"
/**
 ******************************************************************************
 * @file    mqtt_client.c
 * @author  Eshen Wang
 * @version V1.0.0
 * @date    16-Nov-2015
 * @brief   MiCO application demonstrate a MQTT client.
 ******************************************************************************
 * @attention
 *
 * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
 * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
 * TIME. AS A RESULT, MXCHIP Inc. SHALL NOT BE HELD LIABLE FOR ANY
 * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
 * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
 * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
 *
 * <h2><center>&copy; COPYRIGHT 2014 MXCHIP Inc.</center></h2>
 ******************************************************************************
 */
#include "http_server/web_log.h"

#include "main.h"
#include "status_guard.h"
#include "mico.h"
#include "MQTTClient.h"
#include "user_gpio.h"
#include "user_power.h"
#include "user_mqtt_client.h"

/* The moc SDK socket has a five-second receive timeout. Poll first so MQTT's
 * shorter yield deadline is honored and an idle connection is not an error. */
static int tc1_mqtt_read(Network *network, unsigned char *buffer, int length, int timeout_ms)
{
    Timer deadline;
    InitTimer(&deadline);
    countdown_ms(&deadline, timeout_ms > 0 ? timeout_ms : 0);
    int received = 0;
    while (received < length) {
        int remaining = left_ms(&deadline);
        if (remaining <= 0) return received;
        fd_set reads;
        FD_ZERO(&reads); FD_SET(network->my_socket, &reads);
        struct timeval wait = { remaining / 1000, (remaining % 1000) * 1000 };
        int ready = select(network->my_socket + 1, &reads, NULL, NULL, &wait);
        if (!ready) return received;
        if (ready < 0) return -1;
        int count = recv(network->my_socket, buffer + received, length - received, 0);
        if (count <= 0) return -1;
        received += count;
    }
    return received;
}

static void MqttClientThread(mico_thread_arg_t arg);

static void MessageArrived(MessageData *md);
static int ota_service(const char *);
static int service_sender(const char *,const void *,size_t,bool);
static void ota_forget(void);
static char ota_topic[MQTT_TOPIC_MAX];
/* MQTTSubscribe retains this pointer for the entire connection. */
static char command_topic[MQTT_TOPIC_MAX],ha_command_topic[MQTT_TOPIC_MAX];

static OSStatus
MqttMsgPublish(Client *c, const char *topic, char qos, char retained, const unsigned char *msg,
               uint32_t msg_len);



static bool isconnect;
unsigned tc1_mqtt_status = MQTT_DISCONNECTED;
bool tc1_mqtt_cleanup_warning;
static mqtt_account_t account;
static bool synchronous_send, rediscover;
/* Writers only mark pending work. The MQTT owner publishes after commit,
 * outside the config/status locks, and preserves changes during a batch. */
static unsigned ha_refresh_pending;
void UserMqttRefreshState(bool discovery) {
    __atomic_fetch_or(&ha_refresh_pending, discovery ? 3u : 1u, __ATOMIC_RELEASE);
}
/* External periodic producer requests work only. Account/topic/cursors remain
 * exclusively owned by the MQTT worker, including during reconfiguration. */
static bool power_pending,power_turn;
/* sensor-values-v1 is owner-local and independent of retained HA telemetry.
 * Process counters survive MQTT reconnect/account changes. Random failure
 * defers this optional feed; it must not invent a reusable boot identity. */
static uint32_t power_sensor_boot, power_sensor_sequence, power_sensor_at;
static uint32_t power_sensor_wait=5000;
static bool power_sensor_started, power_sensor_turn, power_sensor_metadata;
static int power_sensor_cycle(bool *handled) {
    *handled=false;
    if(!account.mode)return MQTT_SUCCESS;
    if(power_sensor_turn){power_sensor_turn=false;return MQTT_SUCCESS;}
    uint32_t now=mico_rtos_get_time();
    if(power_sensor_started && (uint32_t)(now-power_sensor_at)<power_sensor_wait)return MQTT_SUCCESS;
    *handled=power_sensor_turn=power_sensor_started=true;
    power_sensor_at=now;power_sensor_wait=1000;
    if(!power_sensor_boot && (MicoRandomNumberRead((uint8_t *)&power_sensor_boot,sizeof(power_sensor_boot))!=kNoErr || !power_sensor_boot)) {
        power_sensor_boot=0;return MQTT_SUCCESS;
    }
    tc1_power_sample_t sample;PowerSampleRead(&sample);
    char topic[192];ph_writer w;
    bool metadata=!power_sensor_metadata;
    uint32_t sequence=power_sensor_sequence+1u;if(!sequence)sequence=1;
    if(!mqtt_aux(&account,"sensor",account.mac,topic,sizeof(topic)) ||
       !mqtt_format(topic+strlen(topic),sizeof(topic)-strlen(topic),metadata?"/metadata":"/state") ||
       !phw_init(&w,PH_WIRE_MSGPACK))return MQTT_SUCCESS;
    if(metadata){
        ph_sensor_metadata_begin(&w,account.mac,power_sensor_boot,1);
        ph_sensor_descriptor_write(&w,"power","Power","W");
    }else{
        ph_sensor_snapshot_begin(&w,account.mac,power_sensor_boot,sequence,1);
        ph_sensor_reading_write(&w,"power",sample.watts,sample.valid,sample.age_ms);
    }
    ph_sensor_finish(&w);ph_buf *payload=phw_take(&w);
    if(!payload)return MQTT_SUCCESS;
    int rc=service_sender(topic,payload->buf,payload->len,metadata);ph_buf_free(payload);
    if(rc==MQTT_SUCCESS){
        if(metadata){power_sensor_metadata=true;power_sensor_wait=0;}
        else{power_sensor_sequence=sequence;power_sensor_wait=5000;}
    }
    return rc;
}

static bool power_requested(void) {
    return __atomic_exchange_n(&power_pending, false, __ATOMIC_ACQ_REL);
}
static mico_thread_t mqtt_owner_thread;
static ph_mqtt_startup mqtt_startup;
static int synchronous_error;
static char session_host[128], session_user[64], session_password[257];
static unsigned session_port, session_mode;
static char availability_topic[MQTT_TOPIC_MAX];
static bool session_current(void) {
    return bemfa_primary_enabled() && session_port == (unsigned)user_config->mqtt_port && session_mode == user_config->mqtt_topic_mode &&
        !strcmp(session_host,user_config->mqtt_host) && !strcmp(session_user,user_config->mqtt_user) &&
        !strcmp(session_password,user_config->mqtt_password);
}
static void ha_birth(MessageData *d) {
    if (session_current() && d->message->payloadlen == 6 && !memcmp(d->message->payload,"online",6)) rediscover=true;
}


Client c;  // mqtt client object
Network n;  // socket network for mqtt client
/* One atomic transition owns both start admission and stop intent. Separate
 * flags could lose a concurrent stop between admitting a start and clearing it. */
#define MQTT_THREAD_RUNNING 1u
#define MQTT_THREAD_STOP 2u
static unsigned mqtt_lifecycle;

//static mico_timed_event_t mqtt_client_send_event;

char topic_state[MAX_MQTT_TOPIC_SIZE];
char topic_set[MAX_MQTT_TOPIC_SIZE];  // 婵烇絽娴傞崰妤呭极閸忚偐鈻旈弶鐐存緲閳诲繘鏌ｉ～顒€濮€妞ゆ梹娲樺鍕炊閳轰緡妫楅柣搴ゎ潐濮樸劌鈻撻幋鐐碘枖濠电姵鍑归弳?

mico_timer_t timer_handle;
static char timer_status = 0;



void UserMqttTimerFunc(void *arg) {
    LinkStatusTypeDef LinkStatus = { 0 };

    UNUSED_PARAMETER(arg);
    micoWlanGetLinkStatus(&LinkStatus);
    if (LinkStatus.is_connected != 1) {
        mico_stop_timer(&timer_handle);
        return;
    }
    ph_mqtt_diagnostics diagnostics;ph_mqtt_snapshot(&diagnostics);
    if (!diagnostics.queued[PH_MQTT_SEND] && !diagnostics.inflight[PH_MQTT_SEND]) {

        switch (timer_status) {
            case 0:
                UserMqttHassAutoLed();
                UserMqttHassAutoTotalSocket();
                UserMqttHassAutoChildLock();
                UserMqttHassAutoRebootButton();
                UserMqttHassAutoSoftRebootButton();
                break;
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
                UserMqttHassAuto(timer_status);
                break;
            case 7:
                UserMqttHassAutoPower();
                break;
            default:
                mico_stop_timer(&timer_handle);
                break;
        }
        timer_status++;
    }
}

OSStatus UserMqttDeInit(void) {
    __atomic_fetch_or(&mqtt_lifecycle, MQTT_THREAD_STOP, __ATOMIC_ACQ_REL);
    __atomic_store_n(&isconnect,false,__ATOMIC_RELEASE);
    return kNoErr;
}

void clear_mqtt_msg_send_queue(void) { ph_mqtt_pending_clear(); }
/* Application entrance */
OSStatus UserMqttInit(void) {
    OSStatus err = kNoErr;

    unsigned prior = __atomic_load_n(&mqtt_lifecycle, __ATOMIC_ACQUIRE);
    do {
        if (prior & MQTT_THREAD_RUNNING)
            return kNoErr;
    } while (!__atomic_compare_exchange_n(&mqtt_lifecycle, &prior,
        MQTT_THREAD_RUNNING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    sprintf(topic_set, MQTT_CLIENT_SUB_TOPIC1, str_mac);  // 婵炶揪缍€濞夋洟寮ˉ鐭–闂侀潻闄勫妯侯焽閸愨晜濯存繝濞惧亾閻犳劗鍠撻幏瀣箚瑜滃Σ鐢告煛瀹ュ懏绌块柣锔藉灩缁?
    sprintf(topic_state, MQTT_CLIENT_PUB_TOPIC, str_mac);
    //TODO size:0x800
    int mqtt_thread_stack_size = 0x2000;
    uint32_t mqtt_lib_version = MQTTClientLibVersion();mqtt_log(
            "MQTT client version: [%ld.%ld.%ld]",
            0xFF & (mqtt_lib_version >> 16), 0xFF & (mqtt_lib_version >> 8),
            0xFF & mqtt_lib_version);

    uint32_t generation=ph_mqtt_startup_prepare(&mqtt_startup);
    err = mico_rtos_create_thread(&mqtt_owner_thread, MICO_APPLICATION_PRIORITY, "mqtt_client",
                                  (mico_thread_function_t) MqttClientThread,
                                  mqtt_thread_stack_size, generation);
    ph_mqtt_startup_publish(&mqtt_startup,generation,err==kNoErr);
    if (err != kNoErr) goto exit;

    exit:
    if (kNoErr != err) {
        __atomic_store_n(&mqtt_lifecycle, 0u, __ATOMIC_RELEASE);
    }
    if (kNoErr != err)mqtt_log("ERROR2, app thread exit err: %d kNoErr[%d]", err, kNoErr);
    return err;
}

// publish msg to mqtt server
static OSStatus MqttMsgPublish(Client *c, const char *topic, char qos, char retained,
                               const unsigned char *msg,
                               uint32_t msg_len) {
    OSStatus err = kUnknownErr;
    int ret = 0;
    MQTTMessage publishData = MQTTMessage_publishData_initializer;

    require(topic && msg, exit);

    // upload data qos0
    publishData.qos = (enum QoS) qos;
    publishData.retained = retained;
    publishData.payload = (void *) msg;
    publishData.payloadlen = msg_len;

    ret = ph_mqtt_publish(c, topic, &publishData);

    if (MQTT_SUCCESS == ret) {
        err = kNoErr;
    } else if (PH_MQTT_MORE == ret || PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED == ret) {
        err = ret;
    } else if (MQTT_SOCKET_ERR == ret) {
        err = kConnectionErr;
    } else {
        err = kUnknownErr;
    }

    exit:
    return err;
}

void registerMqttEvents(void) {
if(timer_status !=0){
    mico_stop_timer(&timer_handle);
    }
    timer_status = 0;
    mico_start_timer(&timer_handle);
}

/* Only the MQTT owner publishes. Peripheral/timer and receive workers enqueue,
 * including while the initial snapshot is being assembled. */
static void flush_snapshot(void) {
    /* The common owner drains one outgoing entry between business turns. */
}
/* Each helper is a pure publication enumerator. Its cursor counts only
 * successful sends. Multi-entity legacy helpers resume without replaying
 * earlier writes; no business or queue dispatch runs inside enumeration. */
static unsigned discovery_index,state_index,publication_cursor,publication_seen;
static bool publication_turn,publication_sent;
static int snapshot_step(void) {
    synchronous_send=true;synchronous_error=0;
    publication_turn=true;publication_seen=0;publication_sent=false;
    if(discovery_index<12) {
        switch(discovery_index) {
        case 0:UserMqttHassAutoLed();break;
        case 1:UserMqttHassAutoTotalSocket();break;
        case 2:UserMqttHassAutoChildLock();break;
        case 3:UserMqttHassAutoRebootButton();break;
        case 4:UserMqttHassAutoSoftRebootButton();break;
        case 11:UserMqttHassAutoPower();break;
        default:UserMqttHassAuto(discovery_index-4);break;
        }
    } else if(state_index<10) {
        if(state_index<6)UserMqttSendSocketState(state_index);
        else if(state_index==6)UserMqttSendLedState();
        else if(state_index==7)UserMqttSendTotalSocketState();
        else if(state_index==8)UserMqttSendChildLockState();
        else UserMqttHassPower();
    }
    publication_turn=false;synchronous_send=false;
    if(!synchronous_error && publication_seen) {
        publication_cursor=0;
        if(discovery_index<12)++discovery_index;else ++state_index;
    }
    return synchronous_error;
}
static int cleanup_discovery(void) {
    const char *entities[]={"socket_0","socket_1","socket_2","socket_3","socket_4","socket_5",
        "total_socket","led","childLock","reboot","soft_reboot","power","powerConsumption",
        "startupTime","powerConsumptionToday","powerConsumptionYesterday","energySession"};
    int result=0;
    for (unsigned i=0;i<sizeof(entities)/sizeof(entities[0]);++i) {
        char topic[MQTT_TOPIC_MAX]; mqtt_ha(&account,i<9?"switch":i<11?"button":"sensor",entities[i],topic,sizeof(topic));
        if(MqttMsgPublish(&c,topic,QOS1,1,(const unsigned char *)"",0)!=kNoErr) result=-1;
    }
    return result;
}
static bool cleanup_pending;
static char client_id[192];
static uint32_t snapshot_at;
static bool tc1_topic_bytes(const MQTTString *topic,const char **bytes,size_t *length)
{
    if(!topic)return false;
    if(topic->cstring){*bytes=topic->cstring;*length=strnlen(*bytes,MQTT_TOPIC_MAX);}
    else{if(topic->lenstring.len<=0)return false;*bytes=topic->lenstring.data;*length=(size_t)topic->lenstring.len;}
    return *bytes && *length && *length<MQTT_TOPIC_MAX && !memchr(*bytes,0,*length);
}
static bool ingress_accept(MessageData *d,messageHandler handler) {
    const char *topic;size_t n;
    if(!d || !d->message || !tc1_topic_bytes(d->topicName,&topic,&n))return false;
    MQTTMessage *m=d->message;
    if(!m->payload || !m->payloadlen || m->payloadlen>=MAX_MQTT_DATA_SIZE)return false;
    if(handler==ha_birth)return m->payloadlen==6 && !memcmp(m->payload,"online",6);
    if(m->retained)return false;
    if(n==strlen(command_topic) && !memcmp(topic,command_topic,n))return true;
    return !memchr(m->payload,0,m->payloadlen);
}

static int outgoing(const ph_mqtt_item *message);
static int session_prepare(void *context,ph_mqtt_connection *spec) {
    (void)context;
    TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_DISCONNECTED);__atomic_store_n(&isconnect,false,__ATOMIC_RELEASE);
    LinkStatusTypeDef link={0};micoWlanGetLinkStatus(&link);
    if(cleanup_pending && link.is_connected==1 && !session_current()) {
        bool warning=mqtt_cleanup_reconnect(&c,&account,session_host,session_port,
            session_user,session_password,cleanup_discovery)!=MQTT_SUCCESS;
        TC1_STATUS_WRITE(tc1_mqtt_cleanup_warning=warning);cleanup_pending=false;
    }
    if(!bemfa_primary_enabled() || !user_config->mqtt_host[0] || link.is_connected!=1)return PH_MQTT_PREPARE_WAIT;
    mico_rtos_suspend_all_thread();
    strcpy(session_host,user_config->mqtt_host);strcpy(session_user,user_config->mqtt_user);
    strcpy(session_password,user_config->mqtt_password);session_port=user_config->mqtt_port;
    session_mode=user_config->mqtt_topic_mode;
    mico_rtos_resume_all_thread();
    if(!mqtt_account_init(&account,session_mode,"tc1",session_user,str_mac)) {
        TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_CONFIG_ERROR);return PH_MQTT_PREPARE_WAIT;
    }
    mqtt_leaf(&account,"ota/set",ota_topic,sizeof(ota_topic));
    mqtt_leaf(&account,"command",command_topic,sizeof(command_topic));
    mqtt_leaf(&account,"ha/command",ha_command_topic,sizeof(ha_command_topic));
    mqtt_identity(&account,"",client_id,sizeof(client_id));
    mqtt_leaf(&account,"availability",availability_topic,sizeof(availability_topic));
    if(account.mode){mqtt_leaf(&account,"set",topic_set,sizeof(topic_set));mqtt_leaf(&account,"sensor",topic_state,sizeof(topic_state));}
    else{sprintf(topic_set,MQTT_CLIENT_SUB_TOPIC1,str_mac);sprintf(topic_state,MQTT_CLIENT_PUB_TOPIC,str_mac);}
    rediscover=false;
    *spec=(ph_mqtt_connection){&account,session_host,session_user,session_password,
        client_id,availability_topic,session_port,MQTT_CMD_TIMEOUT,tc1_mqtt_read};
    return PH_MQTT_PREPARE_READY;
}
static bool session_subscription(void *context,unsigned index,ph_mqtt_subscription *out) {
    (void)context;
    if(index==0)*out=(ph_mqtt_subscription){command_topic,QOS1,MessageArrived};
    else if(index==1)*out=(ph_mqtt_subscription){ha_command_topic,QOS1,MessageArrived};
    else if(index==2)*out=(ph_mqtt_subscription){"homeassistant/status",QOS1,ha_birth};
    else if(index==3)*out=(ph_mqtt_subscription){ota_topic,QOS1,MessageArrived};
    else return false;
    return true;
}

static int session_initial(void *context) {
    power_sensor_metadata=false;power_sensor_started=false;
    (void)context;
    PowerSampleInvalidate();
    cleanup_pending=true;discovery_index=state_index=publication_cursor=0;synchronous_error=0;
    snapshot_at=mico_rtos_get_time()-5000u;
    return account.mode?service_mqtt_capabilities(service_sender,&account):MQTT_SUCCESS;
}
static int session_cycle(void *context) {
    (void)context;
    if(!session_current())return PH_MQTT_RECONFIGURE;
    int rc=ota_service(account.base);if(rc!=MQTT_SUCCESS)return rc;
    if(ota_job_active())return MQTT_SUCCESS;
    if(discovery_index==12 && state_index==10) {
        unsigned pending=__atomic_exchange_n(&ha_refresh_pending,0u,__ATOMIC_ACQ_REL);
        if(pending & 2u)rediscover=true;
        else if(pending & 1u){state_index=0;publication_cursor=0;}
    }
    /* Preserve a mid-batch birth until all discovery and initial state items
     * have completed, then replay the entire snapshot for that request. */
    if(rediscover && discovery_index==12 && state_index==10){discovery_index=state_index=publication_cursor=0;rediscover=false;}
    if(discovery_index<12 || state_index<10)return snapshot_step();
    bool sensor_handled;
    int sensor_rc=power_sensor_cycle(&sensor_handled);
    if(sensor_handled)return sensor_rc;
    bool status_due=account.mode && (uint32_t)(mico_rtos_get_time()-snapshot_at)>=5000;
    /* A slow PUBACK can make both producers due on every turn. Alternate
     * completed publication batches instead of starving either one. */
    if(status_due && (!power_turn || !__atomic_load_n(&power_pending,__ATOMIC_ACQUIRE))) {
        /* The turn tracks attempts for fairness; completion time advances only
         * after a real send. MORE keeps the typed snapshot due for retry. */
        power_turn=true;
        int snapshot_rc=service_mqtt_snapshot(service_sender,&account);
        if(snapshot_rc==MQTT_SUCCESS)snapshot_at=mico_rtos_get_time();
        return snapshot_rc;
    }
    if(power_requested()) {
        power_turn=false;state_index=9;publication_cursor=0;
        return snapshot_step();
    }
    return rc;
}
static bool session_initial_ready(void *context) {
    (void)context;
    return discovery_index==12 && state_index==10;
}
static void session_closing(void *context,bool connected,int rc) {
    (void)context;(void)rc;
    __atomic_store_n(&isconnect,false,__ATOMIC_RELEASE);service_mqtt_clear();
    if(!session_current())ota_forget();
    if(connected && !session_current()) {
        bool warning=cleanup_discovery()!=0;
        TC1_STATUS_WRITE(tc1_mqtt_cleanup_warning=warning);cleanup_pending=false;
    }
}
static void session_event(void *context,unsigned event,int rc) {
    (void)context;
    if(event==PH_MQTT_CONNECTING){TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_CONNECTING);}
    else if(event==PH_MQTT_CONNECT_ERROR){TC1_STATUS_WRITE(tc1_mqtt_status=rc==4||rc==5?MQTT_AUTH_FAILED:MQTT_DISCONNECTED);}
    else if(event==PH_MQTT_SUBSCRIBE_ERROR){TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_WAITING_ACL);}
    else if(event==PH_MQTT_READY){__atomic_store_n(&isconnect,true,__ATOMIC_RELEASE);cleanup_pending=true;TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_READY);}
    else if(tc1_mqtt_status==MQTT_READY || tc1_mqtt_status==MQTT_CONNECTING){TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_DISCONNECTED);}
}
static bool session_valid(void *context){(void)context;return bemfa_primary_enabled() && session_current();}
static bool session_stop(void *context){(void)context;return (__atomic_load_n(&mqtt_lifecycle, __ATOMIC_ACQUIRE) & MQTT_THREAD_STOP) != 0;}
void MqttClientThread(mico_thread_arg_t arg) {
    if(ph_mqtt_startup_wait(&mqtt_startup,(uint32_t)arg) &&
       ph_mqtt_owner_init(&account,MAX_MQTT_DATA_SIZE-1,&mqtt_owner_thread,ingress_accept)) {
        ph_mqtt_outgoing(outgoing);
        const ph_mqtt_session_hooks hooks={NULL,session_prepare,session_subscription,
            session_initial,session_cycle,session_closing,session_event,session_stop,session_initial_ready,session_valid};
        ph_mqtt_session_run(&c,&hooks);
    } else {TC1_STATUS_WRITE(tc1_mqtt_status=MQTT_CONFIG_ERROR);}
    ph_mqtt_owner_end();__atomic_store_n(&isconnect,false,__ATOMIC_RELEASE);__atomic_store_n(&mqtt_lifecycle,0u,__ATOMIC_RELEASE);mico_rtos_delete_thread(NULL);
}

// callback, msg received from mqtt server
static bool ota_launch(const char *url) { return ota_server_start((char *)url,NULL,NULL)==kNoErr; }
static void ota_snapshot(char *state,char *error,unsigned *progress) { tc1_ota_snapshot(state,error,progress); }
static int ota_publish(const char *leaf,const void *bytes,size_t length,bool retained) {
    char topic[MQTT_TOPIC_MAX];mqtt_leaf(&account,leaf,topic,sizeof(topic));
    return MqttMsgPublish(&c,topic,QOS1,retained,(const unsigned char *)bytes,length);
}
#include "../../common/ota/mqtt_ota.inc"
static int service_sender(const char *topic,const void *body,size_t length,bool retain){return MqttMsgPublish(&c,topic,QOS1,retain,body,length);}

/* Keep branch-only scratch out of the nested status-command stack. These
 * helpers finish before command dispatch, or run only on the OTA branch. */
static __attribute__((noinline)) bool tc1_command_topic(const char *topic,unsigned length) {
    char command[MQTT_TOPIC_MAX];mqtt_leaf(&account,"command",command,sizeof(command));
    return length==strlen(command) && !memcmp(topic,command,length);
}
static __attribute__((noinline)) void tc1_ota_message(MQTTMessage *message) {
    ota_command(account.base,message->payload,message->payloadlen,message->retained);
}
static void MessageArrived(MessageData *md) {
    MQTTMessage *message=md->message;
    if(session_stop(NULL) || !session_current() || message->retained)return;
    const char *topic;size_t n;if(!tc1_topic_bytes(md->topicName,&topic,&n))return;
    if(tc1_command_topic(topic,n)){
        service_mqtt_input(service_sender,&account,message->payload,message->payloadlen,false);return;
    }
    if(n==strlen(ota_topic) && !memcmp(topic,ota_topic,n)){tc1_ota_message(message);return;}
    if(!ota_job_active() && n==strlen(ha_command_topic) && !memcmp(topic,ha_command_topic,n))ha_command_input(message->payload,message->payloadlen);
}

static int send_owned(const char *topic,const char *body,bool retained) {
    return MqttMsgPublish(&c,topic,QOS1,retained,(const unsigned char *)body,strlen(body));
}
static int outgoing(const ph_mqtt_item *message) {
    if(!session_current())return MQTT_FAILURE;
    int rc=MqttMsgPublish(&c,ph_mqtt_item_topic(message),message->qos,message->retained,ph_mqtt_item_payload(message),message->length);
    /* The external SEND result is fatal; an SDK first cause still wins. */
    return rc<0?ph_mqtt_app_error(PH_MQTT_ERROR_APP_SEND_CONVERSION,rc):rc;
}
OSStatus UserMqttSendTopic(char *topic,char *arg,char retained) {
    if(!topic || !arg || strnlen(topic,MAX_MQTT_TOPIC_SIZE)>=MAX_MQTT_TOPIC_SIZE ||
       strnlen(arg,MAX_MQTT_DATA_SIZE)>=MAX_MQTT_DATA_SIZE)return kParamErr;
    int rc;
    if(ph_mqtt_owner()) {
        if(publication_turn) {
            if(publication_seen++<publication_cursor)return kNoErr;
            if(publication_sent){synchronous_error=PH_MQTT_MORE;return PH_MQTT_MORE;}
        }
        rc=send_owned(topic,arg,retained);
        if(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)rc=PH_MQTT_MORE;
        if(publication_turn && rc==kNoErr){publication_sent=true;++publication_cursor;}
    }
    else rc=ph_mqtt_enqueue_send(topic,arg,strlen(arg),QOS0,retained);
    if(rc!=kNoErr && ph_mqtt_owner() && synchronous_send)synchronous_error=rc;
    return rc;
}

/* Application collect data and seng them to MQTT send queue */
OSStatus UserMqttSend(char *arg) {
    return UserMqttSendTopic(topic_state, arg, 0);
}

//闂備礁鎼ú銈夋偤閵娾晛钃熷┑鍌滈兘闁诲孩顔栭崳顖炲箯閻戣姤鐓曢煫鍥ㄦ尵閿涘秴鈹戦鑺ュ€愭鐐╁亾闂?
static OSStatus tc1_ha_switch_state(const char *entity,bool on){
    char topic[MQTT_TOPIC_MAX],leaf[80];snprintf(leaf,sizeof(leaf),"ha/%s/state",entity);mqtt_leaf(&account,leaf,topic,sizeof(topic));
    return UserMqttSendTopic(topic,on?"ON":"OFF",1);
}
OSStatus UserMqttSendSocketState(char index){if(index<0||index>=6)return kParamErr;char entity[16];snprintf(entity,sizeof(entity),"socket_%u",(unsigned)index);return tc1_ha_switch_state(entity,user_config->socket_status[(unsigned)index]);}
OSStatus UserMqttSendTotalSocketState(void){return tc1_ha_switch_state("total_socket",RelayOut()!=0);}
OSStatus UserMqttSendLedState(void){return tc1_ha_switch_state("led",user_config->power_led_enabled);}
OSStatus UserMqttSendChildLockState(void){return tc1_ha_switch_state("childLock",childLockEnabled);}

#include "ha_discovery.inc"

static bool power_number(char *out, size_t capacity, size_t *used,
                         const char *prefix, double value, unsigned precision)
{
    (void)precision;
    size_t length = strlen(prefix);
    int number;
    if (*used >= capacity || length >= capacity - *used) return false;
    memcpy(out + *used, prefix, length);
    *used += length;
    number = ph_decimal_format_float32(out + *used, capacity - *used, (float)value);
    if (number < 0) return false;
    *used += (size_t)number;
    return true;
}

extern void UserMqttHassPower(void) {
    if (!ph_mqtt_owner()) {
        __atomic_store_n(&power_pending, true, __ATOMIC_RELEASE);
        return;
    }
    if (!__atomic_load_n(&isconnect,__ATOMIC_ACQUIRE) && !synchronous_send) return;
    {
        char state[128],state_topic[MQTT_TOPIC_MAX];ha_json w;IPStatusTypedef ip={0};
        if(micoWlanGetIPStatus(&ip,Station)!=kNoErr)ip.ip[0]=0;ip.ip[sizeof(ip.ip)-1]=0;
        ha_json_init(&w,state,sizeof(state));ha_json_begin(&w,NULL,false);
        ha_json_string(&w,"ip",ip.ip);ha_json_string(&w,"version",VERSION);ha_json_end(&w);
        mqtt_leaf(&account,"ha/state",state_topic,sizeof(state_topic));
        if(!ha_json_done(&w) || UserMqttSendTopic(state_topic,state,1)!=kNoErr)return;
        uint32_t today=p_count>=(uint32_t)user_config->p_count_1_day_ago?p_count-user_config->p_count_1_day_ago:0;
        uint32_t yesterday=user_config->p_count_1_day_ago>=user_config->p_count_2_days_ago?user_config->p_count_1_day_ago-user_config->p_count_2_days_ago:0;
        mico_time_t ms=0; mico_time_get_time(&ms); unsigned sec=ms/1000;
        char payload[512];
        size_t used = 0;
        if (!power_number(payload,sizeof(payload),&used,"{\"power\":",real_time_power/10,3) ||
            !power_number(payload,sizeof(payload),&used,",\"energySession\":",PowerPulseCountToKwh(power_session_pulses),6) ||
            !power_number(payload,sizeof(payload),&used,",\"powerConsumption\":",PowerPulseCountToKwh(p_count),3) ||
            !power_number(payload,sizeof(payload),&used,",\"powerConsumptionToday\":",PowerPulseCountToKwh(today),3) ||
            !power_number(payload,sizeof(payload),&used,",\"powerConsumptionYesterday\":",PowerPulseCountToKwh(yesterday),3) ||
            !mqtt_format(payload+used,sizeof(payload)-used,",\"startupTime\":\"%u - %02u:%02u:%02u\"}",
                         sec/86400,sec/3600%24,sec/60%60,sec%60)) return;
        /* Same owner initialized this exact sensor topic before any publish. */
        char topic[MQTT_TOPIC_MAX];mqtt_leaf(&account,"ha/sensor",topic,sizeof(topic));
        UserMqttSendTopic(topic,payload,1); return;
    }
}

bool UserMqttIsConnect() {
    return __atomic_load_n(&isconnect,__ATOMIC_ACQUIRE) && !session_stop(NULL);
}
