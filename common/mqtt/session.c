/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mico.h"
#include "session.h"
#include <string.h>

static void event(const ph_mqtt_session_hooks *h,unsigned state,int rc) {
    if(h->event)h->event(h->context,state,rc);
}
static int publish_text(Client *client,const char *topic,const char *text) {
    MQTTMessage message={0};message.qos=QOS1;message.retained=1;
    message.payload=(void *)text;message.payloadlen=strlen(text);
    return ph_mqtt_publish(client,topic,&message);
}
static int open_session(Client *client,Network *network,const ph_mqtt_connection *s) {
    memset(network,0,sizeof(*network));network->my_socket=-1;
    ssl_opts options={0};
    int rc=NewNetwork(network,(char *)s->host,s->port,options);
    if(rc!=MQTT_SUCCESS){ph_mqtt_error(PH_MQTT_ERROR_NETWORK,rc);return rc;}
    if(s->read)network->mqttread=s->read;
    rc=ph_mqtt_client_init(client,network,s->command_timeout_ms);
    if(rc!=MQTT_SUCCESS)return rc;
    MQTTPacket_connectData data=MQTTPacket_connectData_initializer;
    data.MQTTVersion=4;data.clientID.cstring=(char *)s->client_id;
    data.username.cstring=s->user[0]?(char *)s->user:NULL;
    data.password.cstring=s->user[0]?(char *)s->password:NULL;
    data.keepAliveInterval=30;data.cleansession=1;
    data.willFlag=1;data.will.topicName.cstring=(char *)s->availability;
    data.will.message.cstring="offline";data.will.qos=1;data.will.retained=1;
    return ph_mqtt_connect(client,&data);
}
static void close_session(Client *client,Network *network) {
    /* Paho deinit does not close the Network. Close it even if init/connect
     * failed. This does not prove closed-kernel close resource reclamation. */
    if(client->isconnected)ph_mqtt_disconnect(client);
    if(network->disconnect)network->disconnect(network);
    ph_mqtt_client_deinit(client);
}
static int announce(Client *client,const mqtt_account_t *account) {
    if(!account->mode)return MQTT_SUCCESS;
    char topic[MQTT_TOPIC_MAX];
    if(!mqtt_aux(account,"discovery",account->mac,topic,sizeof(topic)))return MQTT_FAILURE;
    return publish_text(client,topic,mqtt_announcement(account));
}
static int subscribe_plan(Client *client,const ph_mqtt_session_hooks *h) {
    ph_mqtt_subscription subscription;
    for(unsigned i=0;h->subscription(h->context,i,&subscription);i++) {
        if(i>=MAX_MESSAGE_HANDLERS)return MQTT_FAILURE;
        int rc=ph_mqtt_subscribe(client,subscription.filter,subscription.qos,subscription.handler);
        if(rc!=MQTT_SUCCESS)return rc;
    }
    return MQTT_SUCCESS;
}
void ph_mqtt_session_run(Client *client,const ph_mqtt_session_hooks *h) {
    if(!client || !h || !h->prepare || !h->subscription || !h->initial ||
       !h->cycle || !h->closing || !ph_mqtt_owner())return;
    unsigned attempt=0;
    while(!h->stop || !h->stop(h->context)) {
        ph_mqtt_connection spec={0};
        int ready=h->prepare(h->context,&spec);
        if(ready==PH_MQTT_PREPARE_STOP)break;
        if(ready!=PH_MQTT_PREPARE_READY) { mico_thread_msleep(1000);continue; }
        Network network={0};network.my_socket=-1;
        memset(client,0,sizeof(*client));
        event(h,PH_MQTT_CONNECTING,0);
        int rc=open_session(client,&network,&spec);
        bool connected=rc==MQTT_SUCCESS;
        if(!connected)event(h,PH_MQTT_CONNECT_ERROR,rc);
        if(rc==MQTT_SUCCESS)rc=announce(client,spec.account);
        if(rc==MQTT_SUCCESS) {
            rc=subscribe_plan(client,h);
            if(rc!=MQTT_SUCCESS)event(h,PH_MQTT_SUBSCRIBE_ERROR,rc);
        }
        if(rc==MQTT_SUCCESS) {
            ph_mqtt_turn_begin(4);rc=h->initial(h->context);ph_mqtt_turn_end();
            if(rc==PH_MQTT_MORE)rc=MQTT_SUCCESS;
        }
        if(rc==MQTT_SUCCESS) {attempt=0;event(h,PH_MQTT_READY,0);}
        uint32_t acl_at=mico_rtos_get_time();
        while(rc==MQTT_SUCCESS && (!h->stop || !h->stop(h->context))) {
            rc=ph_mqtt_poll(client);if(rc!=MQTT_SUCCESS)break;
            ph_mqtt_turn_begin(4);rc=h->cycle(h->context);ph_mqtt_turn_end();
            if(rc==PH_MQTT_MORE)rc=MQTT_SUCCESS;
            if(rc!=MQTT_SUCCESS)break;
            if((uint32_t)(mico_rtos_get_time()-acl_at)>=30000) {
                rc=subscribe_plan(client,h);acl_at=mico_rtos_get_time();
                if(rc!=MQTT_SUCCESS)event(h,PH_MQTT_SUBSCRIBE_ERROR,rc);
            }
            if(rc==MQTT_SUCCESS)rc=ph_mqtt_yield(client,100);
            if(rc==MQTT_SUCCESS)rc=ph_mqtt_poll(client);
            mico_thread_msleep(50);
        }
        /* Result send was completed by poll before a rename is noticed here.
         * Clear callbacks queued for this old session before model cleanup. */
        if(rc!=MQTT_SUCCESS)ph_mqtt_error(PH_MQTT_ERROR_SESSION,rc);
        ph_mqtt_pending_clear();
        h->closing(h->context,connected,rc);
        if(connected)publish_text(client,spec.availability,"offline");
        close_session(client,&network);
        event(h,PH_MQTT_STOPPED,rc);
        if(!h->stop || !h->stop(h->context))
            {ph_mqtt_reconnect();mico_thread_msleep(mqtt_retry_seconds(attempt<5?attempt++:5)*1000);}
    }
}
int ph_mqtt_cleanup(Client *client,const mqtt_account_t *account,const char *host,
                    unsigned port,const char *user,const char *password,int (*cleanup)(void)) {
    if(!ph_mqtt_owner())return MQTT_FAILURE;
    char id[192],topic[MQTT_TOPIC_MAX];
    if(!mqtt_identity(account,"",id,sizeof(id)) || !mqtt_leaf(account,"availability",topic,sizeof(topic)))return MQTT_FAILURE;
    ph_mqtt_connection spec={account,host,user,password,id,topic,port,5000,NULL};
    Network network={0};network.my_socket=-1;memset(client,0,sizeof(*client));
    int rc=open_session(client,&network,&spec);
    if(rc==MQTT_SUCCESS) {
        rc=announce(client,account);
        if(rc==MQTT_SUCCESS)rc=cleanup();
        if(publish_text(client,topic,"offline")!=MQTT_SUCCESS)rc=MQTT_FAILURE;
    }
    close_session(client,&network);return rc;
}
