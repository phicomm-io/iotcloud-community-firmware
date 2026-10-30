/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port_internal.h"
#include "../protocol/diagnostics_ids.h"
#include "port.h"
#include "ha_number.h"
#include "boot_trace.h"
#include "../protocol/mqtt/session.h"
#include "../protocol/busy_reply.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static mqtt_account_t account;
static Client client;
static ph_os_thread owner_thread;
static ph_mqtt_startup startup;
static char host[128],user[64],password[128],client_id[192],availability[256],command_topic[256],ha_topic[256];
static unsigned port,discovery,publication,last_fields[2];
static uint32_t sent_at[2],last_sequence[2],ha_serial;
static int application_error;
static bool initialized;
static void command_arrived(MessageData *);
static void ha_arrived(MessageData *);
static void ha_birth(MessageData *);
static bool accept_message(MessageData *d,messageHandler handler) {
 if(!d||!d->message||!d->message->payload||!d->message->payloadlen)return false;
 MQTTMessage *m=d->message;
 if(handler==ha_birth)return m->payloadlen==6&&!memcmp(m->payload,"online",6);
 if(m->retained||m->payloadlen>PH_INPUT_MAX)return false;
 if(handler==ha_arrived)return (m->payloadlen==2&&!memcmp(m->payload,"ON",2))||(m->payloadlen==3&&!memcmp(m->payload,"OFF",3));
 return handler==command_arrived&&ph_binary_format(ph_wire_detect(m->payload,m->payloadlen));
}
static void handle_command(const void *bytes,size_t n,ph_wire_format f) {
 /* Copy borrowed input and capture its namespace before releasing ingress.
  * Reserve a result before mutating GPIO/configuration, as on other models. */
 unsigned char input[PH_INPUT_MAX];if(n>sizeof(input))return;memcpy(input,bytes,n);
 mqtt_target_t target;mqtt_target_set(&target,&account);
 unsigned char *out=malloc(PH_OUTPUT_MAX);
 bool credit=ph_mqtt_input_consumed();
 if(!out||!credit) {
  free(out);unsigned char busy[SERVICE_BUSY_BYTES];char id[65];service_busy_id(input,n,id);
  ph_writer w;struct printbuf b;phw_init_fixed(&w,f,&b,busy,sizeof(busy));phw_begin(&w,NULL,false);
  if(*id)phw_text(&w,"request_id",id);phw_text(&w,"code","busy");phw_text(&w,"message","busy");
  phw_begin(&w,"data",false);phw_end(&w);phw_end(&w);size_t k=phw_finish_fixed(&w);
  int rc=ph_mqtt_receipt_send_bytes(&target,busy,k,NULL,NULL);if(rc<0)application_error=rc;return;
 }
 bl_lock();size_t k=ph_command(&bl_device,input,n,f,f,true,true,out,PH_OUTPUT_MAX);bl_unlock();
 memset(input,0,n);
 if(!k){free(out);application_error=MQTT_FAILURE;return;}
 /* The BK allocator's realloc never shrinks a block. Keeping a 4 KiB
  * reservation for a small receipt otherwise blocks the common sender's
  * live-heap admission until the native low-memory guard reboots the board.
  * Keep the original reservation if this optional compaction cannot allocate. */
 unsigned char *compact=malloc(k);
 if(compact){memcpy(compact,out,k);free(out);out=compact;}
 int rc=ph_mqtt_receipt_send_bytes(&target,out,k,out,free);if(rc<0)application_error=rc;
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
 *spec=(ph_mqtt_connection){&account,host,user,password,client_id,availability,port,3000,NULL};return PH_MQTT_PREPARE_READY;
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
static int publish_cycle(void *unused) {
 (void)unused;if(application_error<0)return application_error;
 char topic[512];unsigned char bytes[PH_OUTPUT_MAX];size_t n;int rc;
 while(discovery<6) {
  n=ha_discovery(discovery,(char*)bytes,sizeof(bytes),topic,sizeof(topic));if(!n)return MQTT_FAILURE;
  rc=ph_mqtt_publish_bytes(topic,bytes,n,true);if(rc!=MQTT_SUCCESS)return rc;++discovery;
 }
 bl_lock();uint32_t sequence=bl_device.sequence;unsigned fields=ph_meter_fields(&bl_device.meter,ph_os_now());unsigned seconds=bl_device.settings.report_seconds;bl_unlock();
 for(unsigned channel=0;channel<2;++channel) {
  if((publication&(1u<<channel))&&sequence==last_sequence[channel]&&fields==last_fields[channel]&&
     (uint32_t)(ph_os_now()-sent_at[channel])<seconds*1000u)continue;
  if(!channel){bl_lock();n=ph_query(&bl_device,"status.get",PH_WIRE_MSGPACK,bytes,sizeof(bytes));bl_unlock();mqtt_leaf(&account,"state",topic,sizeof(topic));}
  else {n=ha_state((char*)bytes,sizeof(bytes));mqtt_leaf(&account,"ha/state",topic,sizeof(topic));}
  if(!n)return MQTT_FAILURE;
  rc=ph_mqtt_publish_bytes(topic,bytes,n,true);if(rc!=MQTT_SUCCESS)return rc;
  publication|=1u<<channel;last_sequence[channel]=sequence;last_fields[channel]=fields;sent_at[channel]=ph_os_now();
 }
 return MQTT_SUCCESS;
}
static int initial(void *unused) {discovery=publication=0;application_error=0;return publish_cycle(unused);}
static bool initial_ready(void *unused) {(void)unused;return discovery==6&&publication==3;}
static void closing(void *unused,bool connected,int rc) {(void)unused;(void)connected;(void)rc;bl_lock();bl_mqtt_status=MQTT_DISCONNECTED;bl_unlock();memset(password,0,sizeof(password));}
static void event(void *unused,unsigned state,int rc) {
 (void)unused;unsigned status=state==PH_MQTT_READY?MQTT_READY:state==PH_MQTT_CONNECTING?MQTT_CONNECTING:
  (rc==4||rc==5)?MQTT_AUTH_FAILED:MQTT_DISCONNECTED;
 bl_lock();bl_mqtt_status=status;if(state==PH_MQTT_STOPPED)bl_mqtt_quiescent=true;bl_unlock();
}
static void owner(beken_thread_arg_t arg) {
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
