#include "MQTTClient.h"
#include <assert.h>
/* Standalone protocol fixture: production admission is provided by its owner. */
int mqtt_buffer_allocation_permit(Client *c,size_t bytes){(void)c;(void)bytes;return 1;}
static unsigned char reply[8];static unsigned length,position;static int scenario;static unsigned requested;static uint32_t clock_ms;
static int read_ack(Network *n,unsigned char *buf,int count,int timeout){(void)n;if(position+count>length){clock_ms+=timeout>0?(unsigned)timeout:1u;return scenario==10?0:-1;}memcpy(buf,reply+position,count);position+=count;return count;}
static int write_sub(Network *n,unsigned char *buf,int count,int timeout){
    (void)n;(void)timeout;unsigned off=2;while(buf[off-1]&128)++off;
    assert((buf[0]&0xf0)==0x80);assert(buf[count-1]==requested);
    unsigned id=(buf[off]<<8)|buf[off+1];
    reply[0]=0x90;reply[1]=scenario==3?4:scenario==4?2:3;reply[2]=id>>8;reply[3]=id;
    if(scenario==2||scenario==7)++reply[3];reply[4]=scenario==1||scenario==7||scenario==8?128:scenario==5?requested+1:requested;reply[5]=1;length=2+reply[1];position=0;
    if(scenario==6)reply[0]=0x91;
    if(scenario==8){reply[1]=4;length=6;}
    if(scenario==9)return -1;
    if(scenario==10)length=0;
    if(scenario==11)length=3;
    if(scenario==12){reply[0]=0xd0;reply[1]=0;length=2;}
    return count;
}
static void handler(MessageData *d){(void)d;}
char expired(Timer *timer){return (int32_t)(clock_ms-(uint32_t)timer->end_time)>=0;}
void countdown_ms(Timer *timer,unsigned int ms){timer->end_time=clock_ms+ms;}
void countdown(Timer *timer,unsigned int seconds){countdown_ms(timer,seconds*1000u);}
int left_ms(Timer *timer){return (int)(timer->end_time-clock_ms);}
void InitTimer(Timer *timer){memset(timer,0,sizeof(*timer));}
uint32_t mico_rtos_get_time(void){return clock_ms;}
int main(void){
    Network n={0};n.mqttread=read_ack;n.mqttwrite=write_sub;
    Client c={0};assert(MQTTClientInit(&c,&n,100)==0);c.isconnected=1;c.keepAliveInterval=30;
    for(requested=0;requested<=2;requested++){
      scenario=0;for(unsigned repeat=0;repeat<100;repeat++)assert(MQTTSubscribe(&c,"test/#",(enum QoS)requested,handler)==0);
      scenario=1;assert(MQTTSubscribe(&c,"test/#",(enum QoS)requested,handler)!=MQTT_SUCCESS);
      if(requested<2){scenario=5;assert(MQTTSubscribe(&c,"test/#",(enum QoS)requested,handler)!=MQTT_SUCCESS);}
    }
    memset(c.messageHandlers,0,sizeof(c.messageHandlers));requested=1;
    for(scenario=1;scenario<=12;++scenario){
      int rc=MQTTSubscribe(&c,"test/#",QOS1,handler);assert(rc!=MQTT_SUCCESS);
#ifdef EXPECT_SUBSCRIBE_DENIED
      if(scenario==1)assert(rc==MQTT_SUBSCRIBE_DENIED);else assert(rc!=MQTT_SUBSCRIBE_DENIED);
#endif
      assert(!c.messageHandlers[0].topicFilter);
    }
    scenario=0;assert(MQTTSubscribe(&c,"test/#",QOS1,handler)==0);assert(c.messageHandlers[0].fp==handler);
    for(unsigned i=0;i<50;++i)assert(MQTTSubscribe(&c,"test/#",QOS1,handler)==0);
    assert(!c.messageHandlers[1].topicFilter); /* polling does not consume slots */
    MQTTClientDeinit(&c);puts("SDK QoS0/1/2 exact wire and SUBACK denial/id/malformed/overgrant/reuse PASS");
}
