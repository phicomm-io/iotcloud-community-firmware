#include "../../common/mqtt_cleanup.h"
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
#include "mico.h"
#include "MQTTClient.h"
#include "user_gpio.h"
#include "user_power.h"
#include "user_mqtt_client.h"
#include "json.h"

typedef struct {
    char topic[MAX_MQTT_TOPIC_SIZE];
    uint32_t generation;
    char qos;
    char retained;

    char data[MAX_MQTT_DATA_SIZE];
    uint32_t datalen;
} mqtt_recv_msg_t, *p_mqtt_recv_msg_t, mqtt_send_msg_t, *p_mqtt_send_msg_t;

static void MqttClientThread(mico_thread_arg_t arg);

static void MessageArrived(MessageData *md);

static OSStatus
MqttMsgPublish(Client *c, const char *topic, char qos, char retained, const unsigned char *msg,
               uint32_t msg_len);

OSStatus UserRecvHandler(void *arg);

void ProcessHaCmd(char *cmd);

bool isconnect = false;
unsigned tc1_mqtt_status = MQTT_DISCONNECTED;
bool tc1_mqtt_cleanup_warning;
static mqtt_account_t account;
static uint32_t generation;
static bool synchronous_send, rediscover;
static int synchronous_error;
static char session_host[32], session_user[64], session_password[257];
static unsigned session_port, session_mode;
static char availability_topic[MQTT_TOPIC_MAX];
static bool session_current(void) {
    return session_port == (unsigned)user_config->mqtt_port && session_mode == user_config->mqtt_topic_mode &&
        !strcmp(session_host,user_config->mqtt_ip) && !strcmp(session_user,user_config->mqtt_user) &&
        !strcmp(session_password,user_config->mqtt_password);
}
static void ha_birth(MessageData *d) {
    if (session_current() && d->message->payloadlen == 6 && !memcmp(d->message->payload,"online",6)) rediscover=true;
}

mico_queue_t mqtt_msg_send_queue = NULL;

Client c;  // mqtt client object
Network n;  // socket network for mqtt client
volatile bool mqtt_thread_should_exit = false;
static volatile bool mqtt_thread_running = false;
static bool mqtt_worker_initialized = false;

static mico_worker_thread_t mqtt_client_worker_thread; /* Worker thread to manage send/recv events */
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
    if (mico_rtos_is_queue_empty(&mqtt_msg_send_queue)) {

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
    mqtt_thread_should_exit = true;
    isconnect = false;
    return kNoErr;
}

void clear_mqtt_msg_send_queue(void) {
if(mqtt_msg_send_queue == NULL){
return;
}
    void *msg = NULL;
    while (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) == false) {
        if (mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &msg, 0) == kNoErr) {
            if (msg) free(msg);  // 闂備焦褰冮敃銉╁棘娴ｇ硶妲堥柛顐ゅ枍缁辨牠鏌涢幇顒佸櫣闁宦板姂閺佸秴鐣濋崟顏嗙礆闂佺绻愮粔鐢垫兜閻樺樊鐓?
        }
    }
}

/* Application entrance */
OSStatus UserMqttInit(void) {
    OSStatus err = kNoErr;

    mico_rtos_suspend_all_thread();
    if (mqtt_thread_running) {
        mico_rtos_resume_all_thread();
        return kNoErr;
    }
    mqtt_thread_running = true;
    mqtt_thread_should_exit = false;
    mico_rtos_resume_all_thread();

    sprintf(topic_set, MQTT_CLIENT_SUB_TOPIC1, str_mac);  // 婵炶揪缍€濞夋洟寮ˉ鐭–闂侀潻闄勫妯侯焽閸愨晜濯存繝濞惧亾閻犳劗鍠撻幏瀣箚瑜滃Σ鐢告煛瀹ュ懏绌块柣锔藉灩缁?
    sprintf(topic_state, MQTT_CLIENT_PUB_TOPIC, str_mac);
    //TODO size:0x800
    int mqtt_thread_stack_size = 0x2000;
    uint32_t mqtt_lib_version = MQTTClientLibVersion();mqtt_log(
            "MQTT client version: [%ld.%ld.%ld]",
            0xFF & (mqtt_lib_version >> 16), 0xFF & (mqtt_lib_version >> 8),
            0xFF & mqtt_lib_version);

    if (mqtt_msg_send_queue == NULL) {
        err = mico_rtos_init_queue(&mqtt_msg_send_queue, "mqtt_msg_send_queue",
                                   sizeof(p_mqtt_send_msg_t),
                                   MAX_MQTT_SEND_QUEUE_SIZE);
        require_noerr_action(err, exit,
                             mqtt_log("ERROR: create mqtt msg send queue err=%d.", err));
    }

    /* The receive worker must exist before the MQTT thread can dispatch to it. */
    if (!mqtt_worker_initialized) {
        err = mico_rtos_create_worker_thread(&mqtt_client_worker_thread,
                                             MICO_APPLICATION_PRIORITY, 0x800, 5);
        require_noerr_string(err, exit, "ERROR: Unable to start the mqtt client worker thread.");
        mqtt_worker_initialized = true;
    }

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "mqtt_client",
                                  (mico_thread_function_t) MqttClientThread,
                                  mqtt_thread_stack_size, 0);
    if (err != kNoErr) goto exit;

    exit:
    if (kNoErr != err) {
        mico_rtos_suspend_all_thread();
        mqtt_thread_running = false;
        mico_rtos_resume_all_thread();
    }
    if (kNoErr != err && !mqtt_worker_initialized && mqtt_msg_send_queue != NULL) {
        clear_mqtt_msg_send_queue();
        mico_rtos_deinit_queue(&mqtt_msg_send_queue);
        mqtt_msg_send_queue = NULL;
    }
    if (kNoErr != err)mqtt_log("ERROR2, app thread exit err: %d kNoErr[%d]", err, kNoErr);
    return err;
}

static OSStatus UserMqttClientRelease(Client *c, Network *n) {
    OSStatus err = kNoErr;

    if (c == NULL || n == NULL) return kParamErr;

    if (c->isconnected) {
        MQTTDisconnect(c);
        c->isconnected = 0;
    }

    if (n->disconnect) {
        n->disconnect(n);
    }

    if (MQTT_SUCCESS != MQTTClientDeinit(c)) {
        mqtt_log("MQTTClientDeinit failed!");
        err = kDeletedErr;
    }
    memset(n, 0, sizeof(Network));

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

    ret = MQTTPublish(c, topic, &publishData);

    if (MQTT_SUCCESS == ret) {
        err = kNoErr;
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
    p_mqtt_send_msg_t msg=NULL;
    while(mico_rtos_pop_from_queue(&mqtt_msg_send_queue,&msg,0)==kNoErr) {
        if(msg->generation==generation && session_current()) {
            int rc=MqttMsgPublish(&c,msg->topic,msg->qos,msg->retained,(const unsigned char *)msg->data,msg->datalen);
            if(rc!=kNoErr) synchronous_error=rc;
        }
        free(msg); msg=NULL;
    }
}
static void all_discovery(void) {
    UserMqttHassAutoLed(); UserMqttHassAutoTotalSocket(); UserMqttHassAutoChildLock();
    UserMqttHassAutoRebootButton(); UserMqttHassAutoSoftRebootButton(); flush_snapshot();
    for (int i=1;i<=SOCKET_NUM;++i) { UserMqttHassAuto(i); flush_snapshot(); }
    UserMqttHassAutoPower(); flush_snapshot();
}
static void all_states(void) {
    for (int i=0;i<SOCKET_NUM;++i) { UserMqttSendSocketState(i); flush_snapshot(); }
    UserMqttSendLedState(); UserMqttSendTotalSocketState(); UserMqttSendChildLockState(); UserMqttHassPower(); flush_snapshot();
}
static int cleanup_discovery(void) {
    const char *entities[]={"socket_0","socket_1","socket_2","socket_3","socket_4","socket_5",
        "total_socket","led","childLock","reboot","soft_reboot","power","powerConsumption",
        "startupTime","powerConsumptionToday","powerConsumptionYesterday"};
    int result=0;
    for (unsigned i=0;i<sizeof(entities)/sizeof(entities[0]);++i) {
        char topic[MQTT_TOPIC_MAX]; mqtt_ha(&account,i<9?"switch":i<11?"button":"sensor",entities[i],topic,sizeof(topic));
        if(MqttMsgPublish(&c,topic,QOS1,1,(const unsigned char *)"",0)!=kNoErr) result=-1;
    }
    return result;
}
void MqttClientThread(mico_thread_arg_t arg) {
    UNUSED_PARAMETER(arg);
    unsigned attempt=0; bool cleanup_pending=false;
    char client_id[192];
    for (;;) {
        if(mqtt_thread_should_exit) break;
        tc1_mqtt_status=MQTT_DISCONNECTED; isconnect=false;
        clear_mqtt_msg_send_queue();
        LinkStatusTypeDef link={0}; micoWlanGetLinkStatus(&link);
        if(cleanup_pending && link.is_connected==1 && !session_current()) {
            tc1_mqtt_cleanup_warning=mqtt_cleanup_reconnect(&c,&account,session_host,session_port,
                session_user,session_password,cleanup_discovery)!=MQTT_SUCCESS;
            cleanup_pending=false;
        }
        if(!user_config->mqtt_ip[0] || link.is_connected!=1) { mico_rtos_thread_sleep(1); continue; }
        mico_rtos_suspend_all_thread();
        strcpy(session_host,user_config->mqtt_ip); strcpy(session_user,user_config->mqtt_user);
        strcpy(session_password,user_config->mqtt_password); session_port=user_config->mqtt_port;
        session_mode=user_config->mqtt_topic_mode; ++generation;
        mico_rtos_resume_all_thread();
        if(!mqtt_account_init(&account,session_mode,"tc1",session_user,str_mac)) {
            tc1_mqtt_status=MQTT_CONFIG_ERROR; mico_rtos_thread_sleep(1); continue;
        }
        mqtt_identity(&account,"",client_id,sizeof(client_id));
        mqtt_leaf(&account,"availability",availability_topic,sizeof(availability_topic));
        if(account.mode) { mqtt_leaf(&account,"set",topic_set,sizeof(topic_set)); mqtt_leaf(&account,"sensor",topic_state,sizeof(topic_state)); }
        else { sprintf(topic_set,MQTT_CLIENT_SUB_TOPIC1,str_mac); sprintf(topic_state,MQTT_CLIENT_PUB_TOPIC,str_mac); }
        memset(&c,0,sizeof(c)); memset(&n,0,sizeof(n)); n.my_socket=-1;
        ssl_opts options={0}; tc1_mqtt_status=MQTT_CONNECTING;
        int rc=NewNetwork(&n,session_host,session_port,options);
        bool connected=false;
        if(rc==MQTT_SUCCESS) rc=MQTTClientInit(&c,&n,MQTT_CMD_TIMEOUT);
        if(rc==MQTT_SUCCESS) {
            MQTTPacket_connectData data=MQTTPacket_connectData_initializer;
            data.MQTTVersion=4; data.clientID.cstring=client_id;
            data.username.cstring=session_user; data.password.cstring=session_password;
            data.keepAliveInterval=30; data.cleansession=1;
            data.willFlag=1; data.will.topicName.cstring=availability_topic;
            data.will.message.cstring="offline"; data.will.qos=1; data.will.retained=1;
            rc=MQTTConnect(&c,&data); connected=rc==MQTT_SUCCESS;
            if(!connected) tc1_mqtt_status=rc==4||rc==5?MQTT_AUTH_FAILED:MQTT_DISCONNECTED;
        }
        if(connected && account.mode) {
            char topic[MQTT_TOPIC_MAX]; mqtt_aux(&account,"discovery",account.mac,topic,sizeof(topic));
            const char *payload=mqtt_announcement(&account);
            rc=MqttMsgPublish(&c,topic,QOS1,1,(const unsigned char *)payload,strlen(payload));
        }
        if(rc==MQTT_SUCCESS) rc=MQTTSubscribe(&c,topic_set,QOS1,MessageArrived);
        if(rc==MQTT_SUCCESS) rc=MQTTSubscribe(&c,"homeassistant/status",QOS1,ha_birth);
        if(connected && rc!=MQTT_SUCCESS) tc1_mqtt_status=MQTT_WAITING_ACL;
        if(rc==MQTT_SUCCESS) {
            cleanup_pending=true; synchronous_send=true; synchronous_error=0; all_discovery(); all_states(); synchronous_send=false;
            rc=synchronous_error;
            if(rc==0) rc=MqttMsgPublish(&c,availability_topic,QOS1,1,(const unsigned char *)"online",6);
        }
        uint32_t acl_check=mico_rtos_get_time(); rediscover=false;
        if(rc==MQTT_SUCCESS) { isconnect=true; tc1_mqtt_status=MQTT_READY; attempt=0; cleanup_pending=true; }
        while(rc==MQTT_SUCCESS && !mqtt_thread_should_exit && session_current()) {
            if(rediscover) {
                synchronous_send=true; synchronous_error=0; all_discovery(); all_states(); synchronous_send=false;
                rc=synchronous_error; rediscover=false;
            }
            p_mqtt_send_msg_t msg=NULL;
            while(rc==MQTT_SUCCESS && mico_rtos_pop_from_queue(&mqtt_msg_send_queue,&msg,0)==kNoErr) {
                if(msg->generation==generation && session_current())
                    rc=MqttMsgPublish(&c,msg->topic,msg->qos,msg->retained,(const unsigned char *)msg->data,msg->datalen);
                free(msg); msg=NULL;
            }
            if(rc==MQTT_SUCCESS && account.mode && (uint32_t)(mico_rtos_get_time()-acl_check)>=30000) {
                rc=MQTTSubscribe(&c,topic_set,QOS1,MessageArrived); acl_check=mico_rtos_get_time();
                if(rc!=MQTT_SUCCESS) tc1_mqtt_status=MQTT_WAITING_ACL;
            }
            if(rc==MQTT_SUCCESS) rc=MQTTYield(&c,100);
            mico_rtos_thread_msleep(50);
        }
        isconnect=false;
        if(connected) {
            if(!session_current()) { tc1_mqtt_cleanup_warning=cleanup_discovery()!=0; cleanup_pending=false; }
            MqttMsgPublish(&c,availability_topic,QOS1,1,(const unsigned char *)"offline",7);
        }
        ++generation; clear_mqtt_msg_send_queue(); UserMqttClientRelease(&c,&n);
        if(tc1_mqtt_status==MQTT_READY || tc1_mqtt_status==MQTT_CONNECTING) tc1_mqtt_status=MQTT_DISCONNECTED;
        if(!mqtt_thread_should_exit) mico_rtos_thread_sleep(mqtt_retry_seconds(attempt++));
    }
    isconnect=false; mqtt_thread_running=false; mico_rtos_delete_thread(NULL);
}

// callback, msg received from mqtt server
static void MessageArrived(MessageData *md) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = NULL;
    MQTTMessage *message = md->message;

    if(!isconnect || !session_current() || message->retained || message->payloadlen >= MAX_MQTT_DATA_SIZE ||
       md->topicName->lenstring.len >= MAX_MQTT_TOPIC_SIZE || memchr(message->payload,0,message->payloadlen)) return;
    p_recv_msg = (p_mqtt_recv_msg_t) calloc(1, sizeof(mqtt_recv_msg_t));
    require_action(p_recv_msg, exit, err = kNoMemoryErr);

    p_recv_msg->datalen = message->payloadlen >= MAX_MQTT_DATA_SIZE ?
                           MAX_MQTT_DATA_SIZE - 1 : message->payloadlen;
    p_recv_msg->generation = generation;
    p_recv_msg->qos = (char) (message->qos);
    p_recv_msg->retained = message->retained;
    int topic_len = md->topicName->lenstring.len >= MAX_MQTT_TOPIC_SIZE ?
                    MAX_MQTT_TOPIC_SIZE - 1 : md->topicName->lenstring.len;
    memcpy(p_recv_msg->topic, md->topicName->lenstring.data, topic_len);
    p_recv_msg->topic[topic_len] = 0;
    memcpy(p_recv_msg->data, message->payload, p_recv_msg->datalen);
    p_recv_msg->data[p_recv_msg->datalen] = 0;

    mqtt_log("MessageArrived topic[%s] data[%s]", p_recv_msg->topic, p_recv_msg->data);
    err = mico_rtos_send_asynchronous_event(&mqtt_client_worker_thread, UserRecvHandler,
                                            p_recv_msg);
    require_noerr(err, exit);

    exit:
    if (err != kNoErr) { mqtt_log("ERROR: Recv data err = %d", err);
        if (p_recv_msg) free(p_recv_msg);
    }
    return;
}

/* Application process MQTT received data */
OSStatus UserRecvHandler(void *arg) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = arg;
    require(p_recv_msg, exit);

    mqtt_log("user get data success! from_topic=[%s], msg=[%ld].", p_recv_msg->topic,
             p_recv_msg->datalen);
    //UserFunctionCmdReceived(0, p_recv_msg->data);

    if(isconnect && session_current() && p_recv_msg->generation==generation && !p_recv_msg->retained && !strcmp(p_recv_msg->topic,topic_set)) ProcessHaCmd(p_recv_msg->data);

    free(p_recv_msg);

    exit:
    return err;
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
        user_config->power_led_enabled = on;
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
        user_config->user[0] = on;
        childLockEnabled = on;
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
        mqtt_leaf(&account,"set",value,sizeof(value)); json_object_object_add(o,"cmd_t",json_object_new_string(value));
        mqtt_format(leaf,sizeof(leaf),"%s/state",entity); mqtt_leaf(&account,leaf,value,sizeof(value));
        if(strcmp(component,"button")) json_object_object_add(o,"stat_t",json_object_new_string(value));
    } else {
        mqtt_leaf(&account,"sensor",value,sizeof(value)); json_object_object_add(o,"state_topic",json_object_new_string(value));
    }
    bool ok=mqtt_format(out,MAX_MQTT_DATA_SIZE,"%s",json_object_to_json_string(o)); json_object_put(o); return ok;
}

OSStatus UserMqttSendTopic(char *topic, char *arg, char retained) {
    OSStatus err = kUnknownErr;
    p_mqtt_send_msg_t p_send_msg = NULL;
    char scoped[MAX_MQTT_TOPIC_SIZE], payload[MAX_MQTT_DATA_SIZE];
    if(strlen(topic)>=MAX_MQTT_TOPIC_SIZE || strlen(arg)>=MAX_MQTT_DATA_SIZE) { if(synchronous_send) synchronous_error=kParamErr; return kParamErr; }
    if(account.mode && !strncmp(topic,"homeassistant/",14)) {
        if(!account_message(topic,arg,scoped,payload)) { if(synchronous_send) synchronous_error=kParamErr; return kParamErr; }
        topic=scoped; arg=payload; retained=1;
    }
    if(mqtt_msg_send_queue == NULL|| (!isconnect && !synchronous_send) || !session_current()){
    return err;
    }

//  mqtt_log("======App prepare to send ![%d]======", MicoGetMemoryInfo()->free_memory);

    /* Send queue is full, pop the oldest */
    if (mico_rtos_is_queue_full(&mqtt_msg_send_queue) == true) {
        mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
        free(p_send_msg);
        p_send_msg = NULL;
    }

    /* Push the latest data into send queue*/
    p_send_msg = (p_mqtt_send_msg_t) calloc(1, sizeof(mqtt_send_msg_t));
    require_action(p_send_msg, exit, err = kNoMemoryErr);

    p_send_msg->generation = generation;
    p_send_msg->qos = account.mode || !strncmp(topic,"homeassistant/",14) ? 1 : 0;
    p_send_msg->retained = retained;
    p_send_msg->datalen = strlen(arg) >= MAX_MQTT_DATA_SIZE ?
                           MAX_MQTT_DATA_SIZE - 1 : strlen(arg);
    memcpy(p_send_msg->data, arg, p_send_msg->datalen);
    p_send_msg->data[p_send_msg->datalen] = 0;
    strncpy(p_send_msg->topic, topic, MAX_MQTT_TOPIC_SIZE - 1);
    p_send_msg->topic[MAX_MQTT_TOPIC_SIZE - 1] = 0;

    err = mico_rtos_push_to_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
    require_noerr(err, exit);

    //mqtt_log("Push user msg into send queue success!");

    exit:
    if (err != kNoErr && p_send_msg) free(p_send_msg);
    if (err != kNoErr && synchronous_send) synchronous_error=err;
    return err;
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
                "\"device_class\":\"outlet\","
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
                "\"device_class\":\"outlet\","
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
                "\"unit_of_measurement\":\"W\","
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
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumption }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,device_name);
        UserMqttSendTopic(topic_buf, send_buf, 1);


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
                "\"unit_of_measurement\":\"kWh\","
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
                "\"unit_of_measurement\":\"kWh\","
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

char topic_buf[128] = {0};
char send_buf[128] = {0};

extern void UserMqttHassPower(void) {
    if(account.mode) {
        uint32_t today=p_count>=(uint32_t)user_config->p_count_1_day_ago?p_count-user_config->p_count_1_day_ago:0;
        uint32_t yesterday=user_config->p_count_1_day_ago>=user_config->p_count_2_days_ago?user_config->p_count_1_day_ago-user_config->p_count_2_days_ago:0;
        mico_time_t ms=0; mico_time_get_time(&ms); unsigned sec=ms/1000;
        char payload[512],topic[MQTT_TOPIC_MAX];
        mqtt_format(payload,sizeof(payload),"{\"power\":%.3f,\"powerConsumption\":%.3f,\"powerConsumptionToday\":%.3f,\"powerConsumptionYesterday\":%.3f,\"startupTime\":\"%u - %02u:%02u:%02u\"}",
            real_time_power/10,PowerPulseCountToKwh(p_count),PowerPulseCountToKwh(today),PowerPulseCountToKwh(yesterday),sec/86400,sec/3600%24,sec/60%60,sec%60);
        mqtt_leaf(&account,"sensor",topic,sizeof(topic)); UserMqttSendTopic(topic,payload,1); return;
    }
    uint32_t today_count = p_count >= (uint32_t) user_config->p_count_1_day_ago
                           ? p_count - (uint32_t) user_config->p_count_1_day_ago : 0;
    uint32_t yesterday_count = user_config->p_count_1_day_ago >= user_config->p_count_2_days_ago
                               ? (uint32_t) (user_config->p_count_1_day_ago -
                                            user_config->p_count_2_days_ago) : 0;

    sprintf(topic_buf, "homeassistant/sensor/%s/power/state", str_mac);
    sprintf(send_buf, "{\"power\":\"%.3f\"}", real_time_power / 10);
    UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumption/state", str_mac);
    sprintf(send_buf, "{\"powerConsumption\":\"%.3f\"}", PowerPulseCountToKwh(p_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    //闁荤姳绶ょ槐鏇㈡偩閼姐倕瀵查柤濮愬€楅崺鐘诲级閳哄倸濮屾い銏″灴瀵噣宕奸弴鐕傜吹
    char up_time[16] = "00:00:00";
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    int past = past_ms / 1000;
    int d = past / 3600 / 24;
    int h = past / 3600 % 24;
    int m = past / 60 % 60;
    int s = past % 60;
    sprintf(up_time, "%d - %02d:%02d:%02d", d, h, m, s);

    sprintf(topic_buf, "homeassistant/sensor/%s/startupTime/state", str_mac);
    sprintf(send_buf, "{\"startupTime\":\"%s\"}", up_time);
    UserMqttSendTopic(topic_buf, send_buf, 0);

//    tc1_log("p_count %ld, p_count_1_day_ago %ld ,p_count_2_days_ago %ld, result %ld",p_count,user_config->p_count_1_day_ago,user_config->p_count_2_days_ago,((p_count-user_config->p_count_1_day_ago)<0?0:(p_count-user_config->p_count_1_day_ago)));
    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionToday/state", str_mac);
    sprintf(send_buf, "{\"powerConsumptionToday\":\"%.3f\"}",
            PowerPulseCountToKwh(today_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionYesterday/state", str_mac);
    sprintf(send_buf, "{\"powerConsumptionYesterday\":\"%.3f\"}",
            PowerPulseCountToKwh(yesterday_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);
}

bool UserMqttIsConnect() {
    return isconnect;
}
