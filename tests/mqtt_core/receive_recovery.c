/* Actual patched Paho: allocation failures are terminal, and TX storage must
 * be reclaimed before interleaved retained ingress/PUBACK waits. */
#define main existing_paho_scenarios
#include "paho.c"
#undef main
static unsigned fault_mode, read_calls;
static int monitored_read(Network *n,unsigned char *p,int count,int timeout) {
    assert(client.buf_size==DEFAULT_SENDBUF_SIZE);
    if (!read_calls++) {
        if (fault_mode==1) heap_capacity=charged_live+16500;
        if (fault_mode==2) fail_allocation=native_allocs+1;
    }
    return read_packet(n,p,count,timeout);
}
static void setup(void) {
    heap_capacity=64000;fail_allocation=0;fault_mode=read_calls=0;rx_at=rx_size=tx_size=0;
    network.mqttread=monitored_read;network.mqttwrite=write_packet;
    assert(ph_mqtt_client_init(&client,&network,50)==0);
    client.isconnected=1;client.keepAliveInterval=30;client.heartbeat_retry_max=1;
    countdown(&client.ping_timer,30);ph_mqtt_session_begin();
}
static void finish(void) { heap_capacity=64000;fail_allocation=0;cleanup();assert(!charged_live); }
int main(void) {
    assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
    char body[714],out[1400];memset(body,'x',sizeof(body)-1);body[sizeof(body)-1]=0;
    memset(out,'y',sizeof(out));
    for(unsigned mode=0;mode<=2;mode++) {
        setup();incoming("unconsumed/state",body,123);fault_mode=mode;
        MQTTMessage m={0};m.qos=QOS1;m.payload=out;m.payloadlen=sizeof(out);
        int rc=ph_mqtt_publish(&client,"tx/state",&m);ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
        if (!mode) assert(rc==0 && client.buf_size==DEFAULT_SENDBUF_SIZE);
        else {
            assert(rc==MQTT_BUFFER_OVERFLOW);
            assert(d.failure.reason==(mode==1?PH_MQTT_CAUSE_HEAP_ADMISSION:PH_MQTT_CAUSE_ALLOCATION));
            assert(d.failure.operation==PH_MQTT_ERROR_PUBLISH && d.failure.rx_state==2);
            /* Header and two remaining-length bytes only: body stays unread. */
            assert(read_calls==3 && rx_at==3);
            assert(ph_mqtt_yield(&client,1)==MQTT_FAILURE); /* failed session cannot resume */
        }
        finish();
        setup();assert(publish()==0);finish(); /* a new session recovers */
    }
    /* The same receive failure must propagate through a SUBACK wait. */
    setup();incoming("unconsumed/state",body,124);fault_mode=1;
    assert(ph_mqtt_subscribe(&client,"new/#",QOS1,handler)==MQTT_BUFFER_OVERFLOW);
    ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
    assert(d.failure.reason==PH_MQTT_CAUSE_HEAP_ADMISSION && d.failure.operation==PH_MQTT_ERROR_SUBSCRIBE);
    assert(read_calls==3 && rx_at==3);finish();
    puts("receive recovery: TX reclaimed, allocation/admission classified, terminal publish/subscribe, reconnect PASS");
}
