#include "../../common/ota/mode.h"
#include "ota_server/signed_adapter.h"
#include "ota_server/ota_server.h"
#include <ctype.h>
#include "../../common/mqtt_cleanup.h"
#include "../../common/mqtt/service.h"
#include "../../common/json_number_checked.h"
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
#include "json.h"

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
static int service_sender(const char *,const char *,bool);
static void ota_forget(void);
static char ota_topic[MQTT_TOPIC_MAX];
/* MQTTSubscribe retains this pointer for the entire connection. */
static char command_topic[MQTT_TOPIC_MAX];

static OSStatus
MqttMsgPublish(Client *c, const char *topic, char qos, char retained, const unsigned char *msg,
               uint32_t msg_len);


void ProcessHaCmd(char *cmd);

static bool isconnect;
unsigned tc1_mqtt_status = MQTT_DISCONNECTED;
bool tc1_mqtt_cleanup_warning;
static mqtt_account_t account;
static bool synchronous_send, rediscover;
/* External periodic producer requests work only. Account/topic/cursors remain
 * exclusively owned by the MQTT worker, including during reconfiguration. */
static bool power_pending,power_turn;
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
    return session_port == (unsigned)user_config->mqtt_port && session_mode == user_config->mqtt_topic_mode &&
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

static bool mqtt_cmd_starts_with(const char *cmd, const char *prefix)
{
    size_t len = strlen(prefix);
    return strncmp(cmd, prefix, len) == 0 && (cmd[len] == 0 || cmd[len] == ' ');
}

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
static bool ingress_accept(MessageData *message,messageHandler handler) {
    return (handler==ha_birth || !message->message->retained) &&
        message->message->payloadlen<MAX_MQTT_DATA_SIZE &&
        !memchr(message->message->payload,0,message->message->payloadlen);
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
    if(!user_config->mqtt_host[0] || link.is_connected!=1)return PH_MQTT_PREPARE_WAIT;
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
    if(account.mode) {
        if(index==0)*out=(ph_mqtt_subscription){command_topic,QOS1,MessageArrived};
        else if(index==1)*out=(ph_mqtt_subscription){"homeassistant/status",QOS1,ha_birth};
        else return false;
    } else {
        if(index==0)*out=(ph_mqtt_subscription){ota_topic,QOS1,MessageArrived};
        else if(index==1)*out=(ph_mqtt_subscription){topic_set,QOS1,MessageArrived};
        else if(index==2)*out=(ph_mqtt_subscription){"homeassistant/status",QOS1,ha_birth};
        else return false;
    }
    return true;
}
static int session_initial(void *context) {
    (void)context;
    cleanup_pending=true;discovery_index=state_index=publication_cursor=0;synchronous_error=0;
    snapshot_at=mico_rtos_get_time()-5000u;
    return account.mode?service_mqtt_capabilities(service_sender,&account):MQTT_SUCCESS;
}
static int session_cycle(void *context) {
    (void)context;
    if(!session_current())return PH_MQTT_RECONFIGURE;
    int rc=ota_service(account.base);if(rc!=MQTT_SUCCESS)return rc;
    if(ota_job_active())return MQTT_SUCCESS;
    /* Preserve a mid-batch birth until all discovery and initial state items
     * have completed, then replay the entire snapshot for that request. */
    if(rediscover && discovery_index==12 && state_index==10){discovery_index=state_index=publication_cursor=0;rediscover=false;}
    if(discovery_index<12 || state_index<10)return snapshot_step();
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
static bool session_valid(void *context){(void)context;return session_current();}
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
static int ota_publish(const char *leaf,const char *text,bool retained) {
    char topic[MQTT_TOPIC_MAX];mqtt_leaf(&account,leaf,topic,sizeof(topic));
    return MqttMsgPublish(&c,topic,QOS1,retained,(const unsigned char *)text,strlen(text));
}
#include "../../common/ota/mqtt_ota.inc"
static int service_sender(const char *topic,const char *body,bool retain){return MqttMsgPublish(&c,topic,QOS1,retain,(const unsigned char *)body,strlen(body));}

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
    /* Subscriptions are live before public READY. This deferred owner path
     * must service admitted commands throughout batched initial publication. */
    if(session_stop(NULL) || !session_current() || message->retained)return;
    const char *topic=md->topicName->cstring;
    unsigned length=strlen(topic);
    if(tc1_command_topic(topic,length)) {
        service_mqtt_input(service_sender,&account,message->payload,message->payloadlen,false);return;
    }
    if(!strcmp(topic,ota_topic)){tc1_ota_message(message);return;}
    if(!ota_job_active() && !strcmp(topic,topic_set))ProcessHaCmd(message->payload);
}

void ProcessHaCmd(char *cmd) {
    mqtt_log("ProcessHaCmd[%s]", cmd);
    char mac[20] = {0};

    if (mqtt_cmd_starts_with(cmd, "set socket")) {
        int i, on;
        if (sscanf(cmd, "set socket %19s %d %d", mac, &i, &on) != 3) return;
        if (strcmp(mac, str_mac) || i<0 || i>=SOCKET_NUM || (on!=0 && on!=1)) return;mqtt_log("set socket[%d] on[%d]", i, on);
        UserRelaySet(i, on);
        UserMqttSendSocketState(i);
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
    } else if (mqtt_cmd_starts_with(cmd, "set led")) {
        int on;
        if (sscanf(cmd, "set led %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac) || (on!=0 && on!=1)) return;mqtt_log("set led on[%d]", on);
        TC1_STATUS_WRITE(user_config->power_led_enabled = on);
        if (RelayOut() && user_config->power_led_enabled) {
            UserLedSet(1);
        } else {
            UserLedSet(0);
        }
        UserMqttSendLedState();
        mico_system_context_update(sys_config);
    } else if (mqtt_cmd_starts_with(cmd, "set total_socket")) {
        int on;
        if (sscanf(cmd, "set total_socket %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac) || (on!=0 && on!=1)) return;mqtt_log("set total_socket on[%d]", on);
        UserRelaySetAll(on);
        int i = 0;
        for (i = 0; i < SOCKET_NUM; i++) {
            UserRelaySet(i, user_config->socket_status[i]);
            UserMqttSendSocketState(i);
        }
        UserMqttSendTotalSocketState();
    }else if (mqtt_cmd_starts_with(cmd, "set childLock")) {
        int on;
        if (sscanf(cmd, "set childLock %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac) || (on!=0 && on!=1)) return;mqtt_log("set childLock on[%d]", on);
        TC1_STATUS_WRITE(user_config->user[0] = on;
        childLockEnabled = on);
        UserMqttSendChildLockState();
        mico_system_context_update(sys_config);
    }else if (mqtt_cmd_starts_with(cmd, "reboot")) {
        if (sscanf(cmd, "reboot %19s", mac) != 1) return;
        if (strcmp(mac, str_mac)) return;
        MicoSystemReboot();  // 缂備焦鏌ㄩ鍛暤閸℃稒鐓傜€广儱鎳忛崕娆撴偣娴ｇ懓鍔ゆい?
    }else if (mqtt_cmd_starts_with(cmd, "soft_reboot")) {
        if (sscanf(cmd, "soft_reboot %19s", mac) != 1) return;
        if (strcmp(mac, str_mac)) return;
        UserSoftReboot();  // 闁哄鍎愰崹鍫曞闯閹间礁瑙︽い鏍ㄧ箥閸熷骸顭?
    }
}

static bool account_message(const char *old_topic, const char *payload, char *topic, char *out) {
    char component[24], mac[20], entity[64], suffix[16], leaf[80];
    if(sscanf(old_topic,"homeassistant/%23[^/]/%19[^/]/%63[^/]/%15s",component,mac,entity,suffix)!=4) return false;
    if(strcmp(mac,str_mac)) return false;
    if(!strcmp(suffix,"state")) {
        if(!strcmp(component,"sensor")) return false; /* power is published as a complete sensor snapshot */
        mqtt_format(leaf,sizeof(leaf),"%s/state",entity);
        const char *last=strrchr(payload,' ');
        if(!strcmp(component,"switch") && last && (!strcmp(last," 0") || !strcmp(last," 1")))payload=last[1]=='1'?"ON":"OFF";
        return mqtt_leaf(&account,leaf,topic,MAX_MQTT_TOPIC_SIZE) && mqtt_format(out,MAX_MQTT_DATA_SIZE,"%s",payload);
    }
    if(strcmp(suffix,"config")) return false;
    json_object *o=json_tokener_parse(payload);
    if(!o) return false;
    mqtt_ha(&account,component,entity,topic,MAX_MQTT_TOPIC_SIZE);
    char value[MQTT_TOPIC_MAX]; mqtt_identity(&account,entity,value,sizeof(value));
    json_object_object_add(o,"uniq_id",json_object_new_string(value));
    json_object_object_add(o,"object_id",json_object_new_string(value));
    json_object *device=json_object_object_get(o,"device");
    if(device) {
        json_object *ids=json_object_new_array(); mqtt_identity(&account,"",value,sizeof(value));
        json_object_array_add(ids,json_object_new_string(value)); json_object_object_add(device,"identifiers",ids);
    }
    mqtt_leaf(&account,"availability",value,sizeof(value)); json_object_object_add(o,"availability_topic",json_object_new_string(value));
    json_object_object_add(o,"retain",json_object_new_boolean(false));
    if(strcmp(component,"sensor")) {
        mqtt_leaf(&account,"command",value,sizeof(value)); json_object_object_add(o,"cmd_t",json_object_new_string(value));
        const char *action=NULL,*field="enabled";int index=-1;
        if(sscanf(entity,"socket_%d",&index)==1 && index>=0 && index<6){action="outlet.set";field="on";}
        else if(!strcmp(entity,"total_socket")){action="outlet.set";field="on";}
        else if(!strcmp(entity,"led"))action="indicator.set";
        else if(!strcmp(entity,"childLock"))action="child_lock.set";
        else if(!strcmp(component,"button"))action="system.reboot";
        if(action){
            char command[192];
            if(!strcmp(component,"button")){
                snprintf(command,sizeof(command),"{\"action\":\"%s\",\"params\":{}}",action);
                json_object_object_add(o,"pl_prs",json_object_new_string(command));
            }else{
                for(unsigned enabled=0;enabled<2;enabled++){
                    if(index>=0)snprintf(command,sizeof(command),"{\"action\":\"%s\",\"params\":{\"%s\":%s,\"index\":%d}}",action,field,enabled?"true":"false",index);
                    else snprintf(command,sizeof(command),"{\"action\":\"%s\",\"params\":{\"%s\":%s}}",action,field,enabled?"true":"false");
                    json_object_object_add(o,enabled?"pl_on":"pl_off",json_object_new_string(command));
                }
                json_object_object_add(o,"stat_on",json_object_new_string("ON"));
                json_object_object_add(o,"stat_off",json_object_new_string("OFF"));
            }
        }
        mqtt_format(leaf,sizeof(leaf),"%s/state",entity); mqtt_leaf(&account,leaf,value,sizeof(value));
        if(strcmp(component,"button")) json_object_object_add(o,"stat_t",json_object_new_string(value));
    } else {
        mqtt_leaf(&account,"sensor",value,sizeof(value)); json_object_object_add(o,"state_topic",json_object_new_string(value));
    }
    mqtt_leaf(&account,"state",value,sizeof(value));
    json_object_object_add(o,"json_attributes_topic",json_object_new_string(value));
    json_object_object_add(o,"json_attributes_template",json_object_new_string("{{ {\"ip\": value_json.ip, \"firmware\": value_json.version} | tojson }}"));
    bool ok=mqtt_format(out,MAX_MQTT_DATA_SIZE,"%s",json_object_to_json_string(o)); json_object_put(o); return ok;
}

static int send_owned(const char *topic,const char *body,bool retained) {
    if(account.mode && !strncmp(topic,"homeassistant/",14)) {
        /* Only the MQTT owner performs JSON/namespace conversion. */
        char *scratch=malloc(MAX_MQTT_TOPIC_SIZE+MAX_MQTT_DATA_SIZE);
        if(!scratch)return synchronous_send?ph_mqtt_app_error(PH_MQTT_ERROR_APP_SEND_CONVERSION,kNoMemoryErr):kNoMemoryErr;
        int rc=kParamErr;
        if(account_message(topic,body,scratch,scratch+MAX_MQTT_TOPIC_SIZE))
            rc=MqttMsgPublish(&c,scratch,QOS1,1,(const unsigned char *)scratch+MAX_MQTT_TOPIC_SIZE,
                              strlen(scratch+MAX_MQTT_TOPIC_SIZE));
        else if(synchronous_send)rc=ph_mqtt_app_error(PH_MQTT_ERROR_APP_SEND_CONVERSION,rc);
        free(scratch);return rc;
    }
    return MqttMsgPublish(&c,topic,account.mode || !strncmp(topic,"homeassistant/",14)?QOS1:QOS0,
                          retained,(const unsigned char *)body,strlen(body));
}
static int outgoing(const ph_mqtt_item *message) {
    if(!session_current())return MQTT_FAILURE;
    int rc=send_owned(ph_mqtt_item_topic(message),ph_mqtt_item_payload(message),message->retained);
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
OSStatus UserMqttSendSocketState(char socket_id) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/socket_%d/state", str_mac, (int) socket_id);
        sprintf(send_buf, "set socket %s %d %d", str_mac, socket_id,
                (int) user_config->socket_status[(int) socket_id]);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendTotalSocketState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/total_socket/state", str_mac);
        sprintf(send_buf, "set total_socket %s %d", str_mac, RelayOut() ? 1 : 0);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendLedState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/led/state", str_mac);
        sprintf(send_buf, "set led %s %d", str_mac, (int) user_config->power_led_enabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendChildLockState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/childLock/state", str_mac);
        sprintf(send_buf, "set childLock %s %d", str_mac, childLockEnabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

//hass mqtt闂備胶鍘ч〃搴㈢濠婂嫭鍙忛柍鍝勬噹閻鏌熺€电孝缂佺姵鐩弻鈩冩媴閸濆嫷鏆悗瑙勬尫缁€渚€顢氶敐澶嬫櫢濞寸姴顑呯粈鍌炴煕閹邦厼绲荤紒鍙夋そ濮婃椽顢曢浣割伓
static void mqtt_json_name(char *out, size_t size, const char *name) {
    json_object *o=json_object_new_string(name);
    if(!o) { out[0]=0; synchronous_error=kNoMemoryErr; return; }
    const char *quoted=json_object_to_json_string(o);
    mqtt_format(out,size,"%.*s",(int)strlen(quoted)-2,quoted+1);
    json_object_put(o);
}
void UserMqttHassAuto(char socket_id) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    socket_id--;
    if(socket_id<0 || socket_id>=SOCKET_NUM) return;
    char socket_name[384];
    mqtt_json_name(socket_name, sizeof(socket_name), user_config->socket_names[(int)socket_id]);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/socket_%d/config", str_mac, socket_id);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"%s\","
                "\"uniq_id\":\"tc1_%s_s%d\","
                "\"object_id\":\"tc1_%s_s%d\","
                "\"stat_t\":\"homeassistant/switch/%s/socket_%d/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set socket %s %d 1\","
                "\"pl_off\":\"set socket %s %d 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                socket_name, str_mac, socket_id,str_mac, socket_id, str_mac, socket_id,
                str_mac,  // 濠电儑缍€椤曆勬叏閻愬灚濯奸柟顖嗗本校MAC闂侀潻闄勫妯侯焽閸愵喖绀嗛柡澶嬪閸ゆ帒霉閻橆喖鈧挾鈧潧鏈敍鎰熺涵鍛箑
                str_mac,
                socket_id, str_mac, socket_id, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoRebootButton(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        // 闂備焦褰冪粔鎾箚鎼淬劌绠板鑸靛姈鐏忥箓姊洪弶璺ㄐら柣?
        sprintf(topic_buf, "homeassistant/button/%s/reboot/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Reboot Device\","
                "\"uniq_id\":\"tc1_%s_reboot\","
                "\"object_id\":\"tc1_%s_reboot\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_prs\":\"reboot %s\","
                "\"device_class\":\"restart\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

void UserMqttHassAutoSoftRebootButton(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        // 闁哄鍎愰崹鍫曞闯閹间礁瑙︽い鏍ㄧ☉閻﹀姊虹粵瀣珝闁告ǜ鍊楃槐?
        sprintf(topic_buf, "homeassistant/button/%s/soft_reboot/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Soft Reboot\","
                "\"uniq_id\":\"tc1_%s_soft_reboot\","
                "\"object_id\":\"tc1_%s_soft_reboot\","
                "\"cmd_t\":\"device/ztc1/%s/set\","
                "\"pl_prs\":\"soft_reboot %s\","
                "\"device_class\":\"restart\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

void UserMqttHassAutoLed(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/led/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"LED Indicator\","
                "\"uniq_id\":\"tc1_%s_led\","
                "\"object_id\":\"tc1_%s_led\","
                "\"stat_t\":\"homeassistant/switch/%s/led/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set led %s 1\","
                "\"pl_off\":\"set led %s 0\","
                "\"entity_category\":\"config\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoChildLock(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/childLock/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Child Lock\","
                "\"uniq_id\":\"tc1_%s_child_lock\","
                "\"object_id\":\"tc1_%s_child_lock\","
                "\"stat_t\":\"homeassistant/switch/%s/childLock/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set childLock %s 1\","
                "\"pl_off\":\"set childLock %s 0\","
                "\"entity_category\":\"config\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoTotalSocket(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/total_socket/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Master Switch\","
                "\"uniq_id\":\"tc1_%s_total_socket\","
                "\"object_id\":\"tc1_%s_total_socket\","
                "\"stat_t\":\"homeassistant/switch/%s/total_socket/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set total_socket %s 1\","
                "\"pl_off\":\"set total_socket %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

//hass mqtt闂備胶鍘ч〃搴㈢濠婂嫭鍙忛柍鍝勬噹閻鏌熺€电孝缂佺姵鐩弻鈩冩媴閸濆嫷鏆悗瑙勬尫缁舵艾鐣峰┑瀣亜闁告繂瀚慨鐢告⒑閸涘﹦鎳勯柛銊ユ健閺佹捇寮妶鍡楊伓
void UserMqttHassAutoPower(void) {
    char device_name[384];
    mqtt_json_name(device_name, sizeof(device_name), sys_config->micoSystemConfig.name);
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = malloc(MAX_MQTT_DATA_SIZE);
    topic_buf = malloc(128);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/sensor/%s/power/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Power\","
                "\"uniq_id\":\"tc1_%s_p\","
                "\"object_id\":\"tc1_%s_p\","
                "\"state_topic\":\"homeassistant/sensor/%s/power/state\","
                "\"unit_of_measurement\":\"W\",\"device_class\":\"power\",\"state_class\":\"measurement\","
                "\"icon\":\"mdi:gauge\","
                "\"value_template\":\"{{ value_json.power }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumption/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Energy Total\","
                "\"uniq_id\":\"tc1_%s_pc\","
                "\"object_id\":\"tc1_%s_pc\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumption/state\","
                "\"unit_of_measurement\":\"kWh\",\"device_class\":\"energy\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumption }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);


        sprintf(topic_buf,"homeassistant/sensor/%s/energySession/config",str_mac);
        snprintf(send_buf,MAX_MQTT_DATA_SIZE,
            "{\"name\":\"Energy Since Boot\",\"uniq_id\":\"tc1_%s_energy_session\",\"state_topic\":\"homeassistant/sensor/%s/energySession/state\",\"unit_of_measurement\":\"kWh\",\"device_class\":\"energy\",\"state_class\":\"total_increasing\",\"value_template\":\"{{ value_json.energySession }}\",\"device\":{\"identifiers\":[\"tc1_%s\"],\"name\":\"%s\",\"model\":\"TC1\",\"manufacturer\":\"PHICOMM\"}}",
            str_mac,str_mac,str_mac,device_name);
        UserMqttSendTopic(topic_buf,send_buf,1);

        sprintf(topic_buf, "homeassistant/sensor/%s/startupTime/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Uptime\","
                "\"uniq_id\":\"tc1_%s_sut\","
                "\"object_id\":\"tc1_%s_sut\","
                "\"state_topic\":\"homeassistant/sensor/%s/startupTime/state\","
                "\"icon\":\"mdi:clock-time-three-outline\","
                "\"entity_category\":\"diagnostic\","
                "\"value_template\":\"{{ value_json.startupTime }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionToday/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Energy Today\","
                "\"uniq_id\":\"tc1_%s_pc_today\","
                "\"object_id\":\"tc1_%s_pc_today\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionToday/state\","
                "\"unit_of_measurement\":\"kWh\",\"device_class\":\"energy\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionToday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionYesterday/config", str_mac);
        snprintf(send_buf, MAX_MQTT_DATA_SIZE,
                "{\"name\":\"Energy Yesterday\","
                "\"uniq_id\":\"tc1_%s_pc_yesterday\","
                "\"object_id\":\"tc1_%s_pc_yesterday\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionYesterday/state\","
                "\"unit_of_measurement\":\"kWh\",\"device_class\":\"energy\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionYesterday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

/* These helpers only prepare owner-local telemetry. A rejected conversion
 * skips that publication; a partially prepared payload never reaches MQTT. */
static bool power_number(char *out, size_t capacity, size_t *used,
                         const char *prefix, double value, unsigned precision)
{
    size_t length = strlen(prefix);
    int number;
    if (*used >= capacity || length >= capacity - *used) return false;
    memcpy(out + *used, prefix, length);
    *used += length;
    number = json_number_format_fixed(out + *used, capacity - *used, value, precision);
    if (number < 0) return false;
    *used += (size_t)number;
    return true;
}

static bool power_legacy(char *out, size_t capacity, const char *key, double value)
{
    int prefix = snprintf(out, capacity, "{\"%s\":\"", key);
    size_t used;
    if (prefix < 0 || (size_t)prefix >= capacity) return false;
    used = (size_t)prefix;
    return power_number(out, capacity, &used, "", value, 3) &&
           mqtt_format(out + used, capacity - used, "\"}");
}

extern void UserMqttHassPower(void) {
    if (!ph_mqtt_owner()) {
        __atomic_store_n(&power_pending, true, __ATOMIC_RELEASE);
        return;
    }
    if (!__atomic_load_n(&isconnect,__ATOMIC_ACQUIRE) && !synchronous_send) return;
    char topic_buf[128], send_buf[128];
    if(account.mode) {
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
        UserMqttSendTopic(topic_state,payload,1); return;
    }
    uint32_t today_count = p_count >= (uint32_t) user_config->p_count_1_day_ago
                           ? p_count - (uint32_t) user_config->p_count_1_day_ago : 0;
    uint32_t yesterday_count = user_config->p_count_1_day_ago >= user_config->p_count_2_days_ago
                               ? (uint32_t) (user_config->p_count_1_day_ago -
                                            user_config->p_count_2_days_ago) : 0;

    sprintf(topic_buf, "homeassistant/sensor/%s/power/state", str_mac);
    if (power_legacy(send_buf,sizeof(send_buf),"power",real_time_power / 10))
        UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumption/state", str_mac);
    if (power_legacy(send_buf,sizeof(send_buf),"powerConsumption",PowerPulseCountToKwh(p_count)))
        UserMqttSendTopic(topic_buf, send_buf, 0);

    char up_time[16];
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    unsigned past = past_ms / 1000;
    unsigned d = past / 3600 / 24;
    unsigned h = past / 3600 % 24;
    unsigned m = past / 60 % 60;
    unsigned s = past % 60;
    if (mqtt_format(up_time,sizeof(up_time),"%u - %02u:%02u:%02u",d,h,m,s) &&
        mqtt_format(send_buf,sizeof(send_buf),"{\"startupTime\":\"%s\"}",up_time)) {
        sprintf(topic_buf, "homeassistant/sensor/%s/startupTime/state", str_mac);
        UserMqttSendTopic(topic_buf, send_buf, 0);
    }

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionToday/state", str_mac);
    if (power_legacy(send_buf,sizeof(send_buf),"powerConsumptionToday",PowerPulseCountToKwh(today_count)))
        UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionYesterday/state", str_mac);
    if (power_legacy(send_buf,sizeof(send_buf),"powerConsumptionYesterday",PowerPulseCountToKwh(yesterday_count)))
        UserMqttSendTopic(topic_buf, send_buf, 0);
}

bool UserMqttIsConnect() {
    return __atomic_load_n(&isconnect,__ATOMIC_ACQUIRE) && !session_stop(NULL);
}
