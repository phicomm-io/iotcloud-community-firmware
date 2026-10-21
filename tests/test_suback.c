#include "MQTTClient.h"
#include <assert.h>
/* Standalone protocol fixture: production admission is provided by its owner. */
int mqtt_buffer_allocation_permit(Client *c,size_t bytes){(void)c;(void)bytes;return 1;}
static unsigned char reply[8];static unsigned length,position;static int scenario;
static int read_ack(Network *n,unsigned char *buf,int count,int timeout){(void)n;(void)timeout;if(position+count>length)return -1;memcpy(buf,reply+position,count);position+=count;return count;}
static int write_sub(Network *n,unsigned char *buf,int count,int timeout){
    (void)n;(void)timeout;unsigned off=2;while(buf[off-1]&128)++off;
    unsigned id=(buf[off]<<8)|buf[off+1];
    reply[0]=0x90;reply[1]=scenario==3?4:scenario==4?2:3;reply[2]=id>>8;reply[3]=id;
    if(scenario==2)++reply[3];reply[4]=scenario==1?128:1;reply[5]=1;length=2+reply[1];position=0;
    return count;
}
static void handler(MessageData *d){(void)d;}
uint32_t mico_rtos_get_time(void){return 0;}
int main(void){
    Network n={0};n.mqttread=read_ack;n.mqttwrite=write_sub;
    Client c={0};assert(MQTTClientInit(&c,&n,100)==0);c.isconnected=1;c.keepAliveInterval=30;
    for(scenario=1;scenario<=4;++scenario){assert(MQTTSubscribe(&c,"test/#",QOS1,handler)!=MQTT_SUCCESS);assert(!c.messageHandlers[0].topicFilter);}
    scenario=0;assert(MQTTSubscribe(&c,"test/#",QOS1,handler)==0);assert(c.messageHandlers[0].fp==handler);
    for(unsigned i=0;i<50;++i)assert(MQTTSubscribe(&c,"test/#",QOS1,handler)==0);
    assert(!c.messageHandlers[1].topicFilter); /* polling does not consume slots */
    MQTTClientDeinit(&c);puts("SDK rejects denied, wrong-ID and malformed SUBACK; polling reuses handler slots");
}
