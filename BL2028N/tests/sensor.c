/* Real S30 serializer/owner with deterministic transport failures and meter. */
#include "../src/sensor.h"
#include "../protocol/mqtt_account.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MQTT_SUCCESS 0
#define MQTT_FAILURE (-1)
#define PH_MQTT_MORE 1
static bool publish_room(const char *topic,size_t bytes){(void)topic;(void)bytes;return true;}
static ph_device bl_device;
static mqtt_account_t account;
static uint32_t sensor_sequence,sensor_at,sensor_wait,now;
static bool sensor_metadata,sensor_started;
static unsigned attempts;
static int send_result;
static uint32_t ph_os_now(void){return now;}
static void bl_lock(void){}
static void bl_unlock(void){}
static int ph_mqtt_publish_bytes(const char *topic,const void *bytes,size_t n,bool retained){
 ++attempts;if(send_result)return send_result;
 printf("%u\t%s\t",retained,topic);const unsigned char *p=bytes;while(n--)printf("%02x",*p++);puts("");return 0;
}
#include "sensor-cycle.inc"
static void cycle(void){unsigned char bytes[1024];char topic[256];unsigned before=attempts;int rc=sensor_cycle(bytes,sizeof(bytes),topic,sizeof(topic));assert(rc==(attempts>before?send_result:0));}
static void meter(bool zero){
 unsigned char frame[]={0x55,0x5a,0x01,0xdd,0xe4,0,0x03,0xe8,0,0x03,0xe8,0,0x03,0xe8,0x01,0xdd,0xe4,0,0x03,0xe8,0x70,0,1,0};
 if(zero)frame[0]=0xf2;
 for(unsigned i=2;i<23;i++)frame[23]+=frame[i];
 for(unsigned i=0;i<24;i++)ph_meter_byte(&bl_device.meter,frame[i],now,bl_device.settings.coefficients);
}
int main(void){
 strcpy(bl_device.mac,"aabbccddeeff");bl_device.boot=99;ph_settings_default(&bl_device.settings);ph_meter_init(&bl_device.meter,0);
 assert(mqtt_account_init(&account,1,PH_MODEL,"meter-owner",bl_device.mac));
 now=1000;cycle();assert(sensor_metadata&&attempts==1);cycle();assert(sensor_sequence==1&&attempts==2);
 meter(false);now+=5000;meter(false);cycle();assert(sensor_sequence==2);
 now+=4999;cycle();assert(attempts==3);now++;cycle();assert(sensor_sequence==3); // exactly stale at 5 seconds
 meter(true);now+=5000;meter(true);send_result=2;cycle();assert(sensor_sequence==3&&attempts==5);
 for(unsigned i=0;i<999;i++){now++;cycle();}assert(attempts==5);
 now++;send_result=0;cycle();assert(sensor_sequence==4&&attempts==6);
 /* Reconnect/rename resends descriptors, preserving the process sequence. */
 assert(mqtt_account_init(&account,1,PH_MODEL,"new-owner",bl_device.mac));sensor_metadata=sensor_started=false;
 cycle();assert(sensor_metadata&&sensor_sequence==4);cycle();assert(sensor_sequence==5);
 sensor_sequence=UINT32_MAX;now+=5000;meter(false);cycle();assert(sensor_sequence==1);
 return 0;
}
