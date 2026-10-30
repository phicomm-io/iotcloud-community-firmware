/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port_internal.h"
#include "../protocol/diagnostics_ids.h"
#include "port.h"
#include "ha_number.h"
#include "boot_trace.h"
#include "sensor.h"
#include "../protocol/mqtt/session.h"
#include "../protocol/busy_reply.h"
#include "../protocol/memory_budget.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static mqtt_account_t account;
static Client client;
static ph_os_thread owner_thread;
static ph_mqtt_startup startup;
static char host[128],user[64],password[128],client_id[192],availability[256],command_topic[256],ha_topic[256];
static char sensor_topic[192];
static unsigned port,discovery,publication,last_fields[2];
static uint32_t sent_at[2],last_sequence[2],ha_serial;
static int application_error;
static uint32_t sensor_retry_at;
static bool sensor_subscribed;
static bool initialized;
/* Sensor publication has its own cursor and starts with retained descriptors. */
static uint32_t sensor_sequence,sensor_at,sensor_wait;
static bool sensor_metadata,sensor_started;
/* Storage belongs to the already-reserved owner stack. It remains alive across
 * deferred receipt retries; native/HA publication must wait for that receipt. */
static unsigned char *workspace;
static void workspace_release(void *unused) {(void)unused;}
static void cache_budget(void) {
 bool pending=ph_mqtt_receipt_pending();
 bl_lock();
 if(pending)ph_status_cache_suspend(&bl_device,true);
 else ph_status_cache_budget(&bl_device,ph_os_free_heap(),SERVICE_MEMORY_RESERVE+PH_OUTPUT_MAX+32u);
 bl_unlock();
}
static void command_arrived(MessageData *);
static void ha_arrived(MessageData *);
static void ha_birth(MessageData *);
static void sensor_grant_arrived(MessageData *);
static bool accept_message(MessageData *d,messageHandler handler) {
 if(!d||!d->message||!d->message->payload||!d->message->payloadlen)return false;
 MQTTMessage *m=d->message;
 if(handler==ha_birth)return m->payloadlen==6&&!memcmp(m->payload,"online",6);
 if(m->retained||m->payloadlen>PH_INPUT_MAX)return false;
 /* Reclaim before the shared ingress allocation, including input delivered
  * during a PUBACK wait. Never release or borrow Paho's receive storage. */
 cache_budget();
 if(handler==ha_arrived)return (m->payloadlen==2&&!memcmp(m->payload,"ON",2))||(m->payloadlen==3&&!memcmp(m->payload,"OFF",3));
 if(handler==sensor_grant_arrived)return ph_wire_detect(m->payload,m->payloadlen)==PH_WIRE_MSGPACK;
 return handler==command_arrived&&ph_binary_format(ph_wire_detect(m->payload,m->payloadlen));
}
static void busy_reply(const mqtt_target_t *target,const void *input,size_t n,ph_wire_format f) {
 unsigned char busy[SERVICE_BUSY_BYTES];char id[65];service_busy_id(input,n,id);
 ph_writer w;struct printbuf b;phw_init_fixed(&w,f,&b,busy,sizeof(busy));phw_begin(&w,NULL,false);
 if(*id)phw_text(&w,"request_id",id);phw_text(&w,"code","busy");phw_text(&w,"message","busy");
 phw_begin(&w,"data",false);phw_end(&w);phw_end(&w);size_t k=phw_finish_fixed(&w);
 int rc=k?ph_mqtt_receipt_send_bytes(target,busy,k,NULL,NULL):MQTT_FAILURE;if(rc<0)application_error=rc;
}
static bool publish_room(const char *topic,size_t n) {
 /* Match the common sender's whole-packet admission, including allocator
  * overhead. This is an early check; Paho retains its fallible live check. */
 return ph_os_free_heap()>=SERVICE_MEMORY_RESERVE+strlen(topic)+n+9u+64u;
}
static bool result_room(const mqtt_target_t *target,size_t n) {
 char topic[MQTT_TOPIC_MAX];
 return mqtt_target_leaf(target,"result",topic,sizeof(topic))&&publish_room(topic,n);
}
static void handle_command(const void *bytes,size_t n,ph_wire_format f) {
 /* Copy borrowed input and capture its namespace before releasing ingress.
  * Reserve a result before mutating GPIO/configuration, as on other models. */
 unsigned char input[PH_INPUT_MAX];if(n>sizeof(input))return;memcpy(input,bytes,n);
 mqtt_target_t target;mqtt_target_set(&target,&account);
 bl_lock();ph_status_cache_suspend(&bl_device,true);bl_unlock();
 /* Input has been copied. Release its heap block before executing or sending;
  * the complete fixed result buffer is already reserved on the owner stack. */
 bool credit=ph_mqtt_input_consumed();
 /* Mutations write at most one fixed receipt data object plus the envelope.
  * Reserve its serialized send headroom before GPIO/Flash can change. */
 if(!credit||!result_room(&target,sizeof(bl_device.receipts[0].data)+SERVICE_BUSY_BYTES)) {
  busy_reply(&target,input,n,f);memset(input,0,n);return;
 }
 bl_lock();size_t k=ph_command(&bl_device,input,n,f,f,true,true,workspace,PH_OUTPUT_MAX);bl_unlock();
 if(!k){memset(input,0,n);application_error=MQTT_FAILURE;return;}
 /* A large read may exceed current wire headroom. Its correlated busy fits
  * the pre-reserved mutation bound; keep completed mutation results intact. */
 if(k>sizeof(bl_device.receipts[0].data)+SERVICE_BUSY_BYTES&&!result_room(&target,k)) {
  busy_reply(&target,input,n,f);memset(input,0,n);return;
 }
 memset(input,0,n);
 int rc=ph_mqtt_receipt_send_bytes(&target,workspace,k,workspace,workspace_release);if(rc<0)application_error=rc;
}
static void command_arrived(MessageData *d) {handle_command(d->message->payload,d->message->payloadlen,ph_wire_detect(d->message->payload,d->message->payloadlen));}
static void ha_arrived(MessageData *d) {
 bool on=d->message->payloadlen==2;unsigned char bytes[256];char id[48];ph_writer w;struct printbuf b;
 snprintf(id,sizeof(id),"ha-%lu-%lu",(unsigned long)bl_device.boot,(unsigned long)++ha_serial);
 phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,bytes,sizeof(bytes));phw_begin(&w,NULL,false);phw_text(&w,"request_id",id);phw_text(&w,"action","outlet.set");
 phw_begin(&w,"params",false);phw_u32(&w,"index",0);phw_bool(&w,"on",on);phw_end(&w);phw_end(&w);
 size_t n=phw_finish_fixed(&w);handle_command(bytes,n,PH_WIRE_MSGPACK);
}
static void ha_birth(MessageData *d) {(void)d;discovery=0;}
/* The existing broker-only grant is intentionally separate from the TMCP
 * admin command route. Its exact account/device topic is subscribed below;
 * neither HTTP nor a UDP command can install a grant. */
static void sensor_grant_arrived(MessageData *d) {
 ph_value root,action,params,value;char peer[13],token[33];const void *raw;size_t bytes;
 int64_t version,boot;
 if(!d||!d->message||d->message->retained||!d->topicName||!MQTTPacket_equals(d->topicName,sensor_topic)||
    !phv_parse(PH_WIRE_MSGPACK,d->message->payload,d->message->payloadlen,&root)||root.kind!=PHV_MAP||
    !phv_keys(&root,"|action||params|")||!phv_get(&root,"action",&action)||!phv_equal_text(&action,"sensor.session.set")||
    !phv_get(&root,"params",&params)||params.kind!=PHV_MAP||!phv_keys(&params,"|v||a1||boot||token|")||
    !phv_get(&params,"v",&value)||!phv_int(&value,&version)||version!=3||
    !phv_get(&params,"boot",&value)||!phv_int(&value,&boot)||boot<1||(uint64_t)boot>UINT32_MAX||
    !phv_get(&params,"a1",&value)||!phv_text(&value,peer,sizeof(peer))||strlen(peer)!=12||
    !strcmp(peer,bl_device.mac)||!phv_get(&params,"token",&value)||!phv_bytes(&value,&raw,&bytes)||bytes!=16)return;
 for(unsigned i=0;i<12;++i)if(!((peer[i]>='0'&&peer[i]<='9')||(peer[i]>='a'&&peer[i]<='f')))return;
 static const char hex[]="0123456789abcdef";
 unsigned bits=0;for(unsigned i=0;i<16;++i){unsigned v=((const uint8_t *)raw)[i];bits|=v;token[2*i]=hex[v>>4];token[2*i+1]=hex[v&15];}token[32]=0;
 if(bits){bl_lock();if(!bl_ota_pending)(void)bl_sensor_grant(peer,token,(uint32_t)boot);bl_unlock();}
 memset(token,0,sizeof(token));
}
static bool session_valid(void *unused) {
 (void)unused;bl_lock();bool valid=!bl_ota_pending&&Main_IsConnectedToWiFi()&&!PH_BL_MqttTlsEnabled()&&port==(unsigned)CFG_GetMQTTPort()&&
 !strcmp(host,CFG_GetMQTTHost())&&!strcmp(user,CFG_GetMQTTUserName())&&!strcmp(password,CFG_GetMQTTPass());bl_unlock();return valid;
}
static int prepare(void *unused,ph_mqtt_connection *spec) {
 (void)unused;bl_lock();
 if(bl_ota_pending||!Main_IsConnectedToWiFi()||!*CFG_GetMQTTHost()){bl_mqtt_status=MQTT_DISCONNECTED;bl_unlock();return PH_MQTT_PREPARE_WAIT;}
 if(PH_BL_MqttTlsEnabled()||strlen(CFG_GetMQTTHost())>=sizeof(host)||strlen(CFG_GetMQTTPass())>=sizeof(password)||
  !mqtt_account_init(&account,MQTT_TOPIC_TMCP,PH_MODEL,CFG_GetMQTTUserName(),bl_device.mac)) {
  bl_mqtt_status=MQTT_CONFIG_ERROR;bl_unlock();return PH_MQTT_PREPARE_WAIT;
 }
 bl_mqtt_quiescent=false;
 strcpy(host,CFG_GetMQTTHost());strcpy(user,CFG_GetMQTTUserName());strcpy(password,CFG_GetMQTTPass());port=CFG_GetMQTTPort();
 bl_unlock();
 mqtt_identity(&account,"",client_id,sizeof(client_id));mqtt_leaf(&account,"availability",availability,sizeof(availability));
 mqtt_leaf(&account,"command",command_topic,sizeof(command_topic));mqtt_leaf(&account,"ha/set",ha_topic,sizeof(ha_topic));
 char leaf[32];snprintf(leaf,sizeof(leaf),"%s/command",account.mac);
 if(!mqtt_aux(&account,"sensor",leaf,sensor_topic,sizeof(sensor_topic)))return PH_MQTT_PREPARE_WAIT;
 *spec=(ph_mqtt_connection){&account,host,user,password,client_id,availability,port,3000,NULL,bl_device.version,"full"};return PH_MQTT_PREPARE_READY;
}
static bool subscription(void *unused,unsigned i,ph_mqtt_subscription *s) {
 (void)unused;
 if(i==0)*s=(ph_mqtt_subscription){command_topic,QOS1,command_arrived};
 else if(i==1)*s=(ph_mqtt_subscription){ha_topic,QOS1,ha_arrived};
 else if(i==2)*s=(ph_mqtt_subscription){"homeassistant/status",QOS1,ha_birth};
 else return false;return true;
}
/* JSON is used exclusively for Home Assistant discovery and state. */
static size_t ha_discovery(unsigned i,char *out,size_t cap,char *topic,size_t topic_cap) {
 static const char *entity[]={"outlet_0","voltage","current","power","energy","meter_valid"};
 static const char *component[]={"switch","sensor","sensor","sensor","sensor","binary_sensor"};
 static const char *key[]={"on","voltage","current","power_watts","energy_kwh","meter_valid"};
 static const char *unit[]={"","V","A","W","kWh",""};
 static const char *device_class[]={"outlet","voltage","current","power","energy","connectivity"};
 char unique[224],device_id[192],state[256],command[256],extra[384]="";
 if(i>=6||!mqtt_ha(&account,component[i],entity[i],topic,topic_cap)||!mqtt_identity(&account,entity[i],unique,sizeof(unique))||
 !mqtt_identity(&account,"",device_id,sizeof(device_id))||!mqtt_leaf(&account,"ha/state",state,sizeof(state))||!mqtt_leaf(&account,"ha/set",command,sizeof(command)))return 0;
 if(!i)snprintf(extra,sizeof(extra),",\"command_topic\":\"%s\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"state_on\":\"ON\",\"state_off\":\"OFF\"",command);
 else if(i==5)strcpy(extra,",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");
 else snprintf(extra,sizeof(extra),",\"unit_of_measurement\":\"%s\",\"state_class\":\"%s\"",unit[i],i==4?"total_increasing":"measurement");
 int n=snprintf(out,cap,"{\"name\":\"%s\",\"unique_id\":\"%s\",\"object_id\":\"%s\",\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\",\"availability_topic\":\"%s\",\"device_class\":\"%s\",\"retain\":false%s,\"device\":{\"identifiers\":[\"%s\"],\"name\":\"S30-V1B\",\"manufacturer\":\"ORVIBO\",\"model\":\"S30-V1B\",\"sw_version\":\"%s\"}}",
 entity[i],unique,unique,state,key[i],availability,device_class[i],extra,device_id,bl_device.version);
 return n>0&&(size_t)n<cap?(size_t)n:0;
}
static size_t ha_state(char *out,size_t cap) {
 char numbers[4][32];bool on;unsigned fields;double value[4];
 bl_lock();fields=ph_meter_fields(&bl_device.meter,ph_os_now());on=bl_device.relay;
 for(unsigned i=0;i<3;++i)value[i]=(fields&(1u<<i))?bl_device.meter.value[i]:NAN;
 value[3]=bl_device.meter.energy_kwh;bl_unlock();
 for(unsigned i=0;i<4;++i)ph_ha_number(numbers[i],value[i]);
 int n=snprintf(out,cap,"{\"on\":\"%s\",\"voltage\":%s,\"current\":%s,\"power_watts\":%s,\"energy_kwh\":%s,\"meter_valid\":\"%s\",\"version\":\"%s\"}",on?"ON":"OFF",numbers[0],numbers[1],numbers[2],numbers[3],fields==7?"ON":"OFF",bl_device.version);
 return n>0&&(size_t)n<cap?(size_t)n:0;
}
static int sensor_cycle(void *bytes,size_t capacity,char *topic,size_t topic_capacity){
 uint32_t now=ph_os_now();
 if(sensor_started&&(uint32_t)(now-sensor_at)<sensor_wait)return MQTT_SUCCESS;
 sensor_started=true;sensor_at=now;sensor_wait=1000;
 bool metadata=!sensor_metadata;uint32_t sequence=sensor_sequence+1u;if(!sequence)sequence=1;
 if(!mqtt_aux(&account,"sensor",account.mac,topic,topic_capacity)||
    !mqtt_format(topic+strlen(topic),topic_capacity-strlen(topic),metadata?"/metadata":"/state"))return MQTT_FAILURE;
 ph_writer w;struct printbuf buffer;
 if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,capacity))return MQTT_FAILURE;
 bl_lock();if(metadata)ph_meter_sensor_metadata(&bl_device,&w);else ph_meter_sensor_state(&bl_device,&w,sequence,now);bl_unlock();
 size_t n=phw_finish_fixed(&w);if(!n)return MQTT_FAILURE;
 if(!publish_room(topic,n))return PH_MQTT_MORE;
 int rc=ph_mqtt_publish_bytes(topic,bytes,n,metadata);
 if(rc==MQTT_SUCCESS){if(metadata){sensor_metadata=true;sensor_started=false;}else{sensor_sequence=sequence;sensor_wait=5000;}}
 return rc;
}
static int publish_cycle(void *unused) {
 (void)unused;if(application_error<0)return application_error;
 if(ph_mqtt_receipt_pending())return PH_MQTT_MORE;
 cache_budget();
 if(!sensor_subscribed&&(int32_t)(ph_os_now()-sensor_retry_at)>=0) {
  int sub=ph_mqtt_subscribe_optional(&client,sensor_topic,QOS1,sensor_grant_arrived);
  if(sub==MQTT_SUCCESS)sensor_subscribed=true;
  else if(sub==MQTT_SUBSCRIPTION_REJECTED)sensor_retry_at=ph_os_now()+60000u;
  else return sub;
 }
 char topic[512];unsigned char *bytes=workspace;size_t n;int rc;bool waiting=false;
 while(discovery<6) {
  n=ha_discovery(discovery,(char*)bytes,PH_OUTPUT_MAX,topic,sizeof(topic));if(!n)return MQTT_FAILURE;
  if(!publish_room(topic,n)){waiting=true;break;}
  rc=ph_mqtt_publish_bytes(topic,bytes,n,true);if(rc!=MQTT_SUCCESS)return rc;++discovery;
 }
 bl_lock();uint32_t sequence=bl_device.sequence;unsigned fields=ph_meter_fields(&bl_device.meter,ph_os_now());unsigned seconds=bl_device.settings.report_seconds;bl_unlock();
 for(unsigned channel=0;channel<2;++channel) {
  if((publication&(1u<<channel))&&sequence==last_sequence[channel]&&fields==last_fields[channel]&&
     (uint32_t)(ph_os_now()-sent_at[channel])<seconds*1000u)continue;
  if(!channel){bl_lock();n=ph_query(&bl_device,"status.get",PH_WIRE_MSGPACK,bytes,PH_OUTPUT_MAX);bl_unlock();mqtt_leaf(&account,"state",topic,sizeof(topic));}
  else {n=ha_state((char*)bytes,PH_OUTPUT_MAX);mqtt_leaf(&account,"ha/state",topic,sizeof(topic));}
  if(!n)return MQTT_FAILURE;
  /* Telemetry remains reproducible from device state. Do not create a shared
   * deferred-send episode for a known oversized state packet: small HA/state
   * updates and correlated command results can still make progress. */
  if(!publish_room(topic,n)){waiting=true;continue;}
  rc=ph_mqtt_publish_bytes(topic,bytes,n,true);if(rc!=MQTT_SUCCESS)return rc;
  publication|=1u<<channel;last_sequence[channel]=sequence;last_fields[channel]=fields;sent_at[channel]=ph_os_now();
 }
 rc=sensor_cycle(bytes,PH_OUTPUT_MAX,topic,sizeof(topic));
 if(rc!=MQTT_SUCCESS)return rc;
 return waiting?PH_MQTT_MORE:MQTT_SUCCESS;
}
static int initial(void *unused) {discovery=publication=0;application_error=0;sensor_metadata=sensor_started=false;sensor_subscribed=false;sensor_retry_at=ph_os_now();return publish_cycle(unused);}
/* Optional discovery/full-state packets can wait for heap recovery while the
 * subscribed connection and its compact HA state are already operational. */
static bool initial_ready(void *unused) {(void)unused;return (publication&2u)!=0;}
static void closing(void *unused,bool connected,int rc) {(void)unused;(void)connected;(void)rc;cache_budget();sensor_subscribed=false;sensor_retry_at=0;bl_lock();bl_mqtt_status=MQTT_DISCONNECTED;memset(bl_auth.mqtt,0,sizeof(bl_auth.mqtt));bl_unlock();memset(password,0,sizeof(password));}
static void event(void *unused,unsigned state,int rc) {
 (void)unused;bl_lock();
 if(state==PH_MQTT_READY)bl_mqtt_status=MQTT_READY;
 else if(state==PH_MQTT_CONNECTING)bl_mqtt_status=MQTT_CONNECTING;
 else if(state==PH_MQTT_SUBSCRIBE_ERROR&&rc==MQTT_SUBSCRIPTION_REJECTED)bl_mqtt_status=MQTT_WAITING_ACL;
 else if(state==PH_MQTT_CONNECT_ERROR&&(rc==4||rc==5))bl_mqtt_status=MQTT_AUTH_FAILED;
 else if(state!=PH_MQTT_STOPPED||(bl_mqtt_status!=MQTT_WAITING_ACL&&bl_mqtt_status!=MQTT_AUTH_FAILED))bl_mqtt_status=MQTT_DISCONNECTED;
 if(state==PH_MQTT_STOPPED)bl_mqtt_quiescent=true;bl_unlock();
}
static void owner(beken_thread_arg_t arg) {
 unsigned char storage[PH_OUTPUT_MAX];workspace=storage;
 PH_BOOT_STAGE("mqtt-thread");
 if(!ph_mqtt_startup_wait(&startup,(uint32_t)arg)||!ph_mqtt_owner_init(&account,PH_INPUT_MAX,&owner_thread,accept_message)){rtos_delete_thread(NULL);return;}
 ph_os_store(&initialized,true);
 PH_BOOT_STAGE("mqtt-initialized");
 ph_mqtt_session_hooks hooks={NULL,prepare,subscription,initial,publish_cycle,closing,event,NULL,initial_ready,session_valid};
 ph_mqtt_session_run(&client,&hooks);ph_mqtt_owner_end();rtos_delete_thread(NULL);
}
void bl_mqtt_start(void) {
 uint32_t generation=ph_mqtt_startup_prepare(&startup);
 int rc=rtos_create_thread(&owner_thread,BEKEN_APPLICATION_PRIORITY,"openiot-mqtt",owner,16384,(beken_thread_arg_t)generation);
 PH_BOOT_RESULT("mqtt-created",rc);
 ph_mqtt_startup_publish(&startup,generation,rc==0);
}
void bl_mqtt_diagnostics(ph_writer *w) {
 ph_mqtt_diagnostics d={0};if(ph_os_load(&initialized))ph_mqtt_snapshot(&d);
 ph_diag_uint(w,PH_DIAG_MQTT_ERRORS,d.errors);ph_diag_int(w,PH_DIAG_MQTT_LAST_ERROR,d.last_error);
 ph_diag_uint(w,PH_DIAG_SERVICE_MEMORY_MQTT_PENDING,d.bytes);ph_diag_uint(w,PH_DIAG_SERVICE_MEMORY_MQTT_PEAK,d.peak_bytes);
}
