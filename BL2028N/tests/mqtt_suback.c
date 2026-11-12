/* Actual Paho + owner adapter: only an exact optional refusal is nonfatal. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Including the implementation permits an injected internal failure to race
 * the otherwise valid ACK, without adding a production testing API. */
#include "../protocol/mqtt/client.c"

static unsigned char answer[8];
static size_t answer_size, answer_at;
static int grant, bad_id, bad_flags, extra, read_error, write_error, latch;
static bool allocation_failure;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    return allocation_failure ? NULL : __real_malloc(n);
}

static int scripted_write(Network *n,unsigned char *p,int count,int timeout) {
    (void)n;(void)timeout;
    if(write_error)return -1;
    assert(p[0]==0x82);
    size_t at=1;while(p[at++]&0x80){}
    unsigned id=((unsigned)p[at]<<8)|p[at+1];
    if(bad_id)++id;
    answer[0]=bad_flags?0x91:0x90;answer[1]=extra?4:3;
    answer[2]=(unsigned char)(id>>8);answer[3]=(unsigned char)id;
    answer[4]=(unsigned char)grant;answer[5]=1;
    answer_size=extra?6:5;answer_at=0;
    return count;
}
static int scripted_read(Network *n,unsigned char *p,int count,int timeout) {
    (void)n;(void)timeout;
    if(read_error<0)return -1;
    if(read_error>0){ph_os_sleep(1);return 0;}
    assert((size_t)count<=answer_size-answer_at);
    memcpy(p,answer+answer_at,(size_t)count);answer_at+=(size_t)count;
    if(latch&&answer_at==answer_size)primary_port.failed=true;
    return count;
}
static void delivered(MessageData *m){(void)m;assert(!"unexpected delivery");}

static void check(const char *name,bool optional,int value,int id,int flags,int more,
                  int read_fault,int write_fault,int internal,bool oom,enum QoS requested,int expected) {
    Client c;Network n={0};
    n.mqttread=scripted_read;n.mqttwrite=scripted_write;
    grant=value;bad_id=id;bad_flags=flags;extra=more;
    read_error=read_fault;write_error=write_fault;latch=internal;
    allocation_failure=false;
    assert(ph_mqtt_client_init(&c,&n,25)==MQTT_SUCCESS);c.isconnected=1;
    ph_mqtt_session_begin();
    ph_mqtt_diagnostics before,after;ph_mqtt_snapshot(&before);
    char large[701];memset(large,'a',sizeof(large)-1);large[sizeof(large)-1]=0;
    allocation_failure=oom;
    const char *topic=oom?large:"optional/sensor/command";
    int rc=optional?ph_mqtt_subscribe_optional(&c,topic,requested,delivered):
                    ph_mqtt_subscribe(&c,topic,requested,delivered);
    allocation_failure=false;
    if(rc!=expected)fprintf(stderr,"%s: actual=%d expected=%d cause=%u failed=%u\n",name,rc,expected,primary_port.cause,primary_port.failed);
    assert(rc==expected);
    ph_mqtt_snapshot(&after);
    bool benign=expected==MQTT_SUCCESS||(optional&&expected==MQTT_SUBSCRIPTION_REJECTED);
    assert(after.errors==before.errors+(benign?0u:1u));
    assert(after.protocol_depth==0&&after.operation==0);
    assert((primary_port.subscriptions[0].filter!=NULL)==(rc==MQTT_SUCCESS));
    assert((c.messageHandlers[0].topicFilter!=NULL)==(rc==MQTT_SUCCESS));
    assert(ph_mqtt_client_deinit(&c)==MQTT_SUCCESS);
    printf("PASS %s rc=%d errors=%u\n",name,rc,after.errors-before.errors);
}

int main(void) {
    setbuf(stdout,NULL);
    mqtt_account_t account;ph_os_thread owner=pthread_self();
    assert(mqtt_account_init(&account,MQTT_TOPIC_TMCP,"s30-v1b","test-user","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,1200,&owner,NULL));
    check("optional exact refusal",true,128,0,0,0,0,0,0,false,QOS1,MQTT_SUBSCRIPTION_REJECTED);
    check("required refusal remains error",false,128,0,0,0,0,0,0,false,QOS1,MQTT_SUBSCRIPTION_REJECTED);
    check("grant qos0",true,0,0,0,0,0,0,0,false,QOS1,MQTT_SUCCESS);
    check("grant qos1",true,1,0,0,0,0,0,0,false,QOS1,MQTT_SUCCESS);
    check("grant qos2 requested",true,2,0,0,0,0,0,0,false,QOS2,MQTT_SUCCESS);
    check("grant exceeds requested qos",true,2,0,0,0,0,0,0,false,QOS1,MQTT_FAILURE);
    check("illegal positive grant",true,3,0,0,0,0,0,0,false,QOS2,MQTT_FAILURE);
    check("illegal 0x81",true,129,0,0,0,0,0,0,false,QOS1,MQTT_FAILURE);
    check("refusal wrong packet id",true,128,1,0,0,0,0,0,false,QOS1,MQTT_FAILURE);
    check("refusal bad flags",true,128,0,1,0,0,0,0,false,QOS1,MQTT_FAILURE);
    check("refusal extra return code",true,128,0,0,1,0,0,0,false,QOS1,MQTT_FAILURE);
    check("read IO failure",true,128,0,0,0,-1,0,0,false,QOS1,MQTT_SOCKET_ERR);
    check("ACK timeout",true,128,0,0,0,1,0,0,false,QOS1,MQTT_FAILURE);
    check("write IO failure",true,128,0,0,0,0,1,0,false,QOS1,MQTT_FAILURE);
    check("internal failure wins refusal",true,128,0,0,0,0,0,1,false,QOS1,MQTT_FAILURE);
    check("real allocation failure",true,128,0,0,0,0,0,0,true,QOS1,MQTT_BUFFER_OVERFLOW);
    ph_mqtt_owner_end();
    puts("PASS 16 strict SUBACK owner/Paho cases");return 0;
}
