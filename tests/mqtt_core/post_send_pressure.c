/* Reproduce the observed A1 failure after a successful socket write, before
 * the SDK restores its small ACK/TX workspace. No sockets or device writes. */
#define main existing_paho_scenarios
#include "paho.c"
#undef main
#include "../../common/memory_budget.h"
static unsigned wire_written, post_send_fault;
static int pressure_after_write(Network *n,unsigned char *p,int count,int timeout) {
    int rc=write_packet(n,p,count,timeout);
    if(rc>0) {
        wire_written+=(unsigned)rc;
        /* Model the network stack retaining a transmitted copy. Leave too little
         * headroom to admit a replacement workspace under the soft reserve. */
        if(!post_send_fault)heap_capacity=charged_live+SERVICE_MEMORY_RESERVE-1800u;
        else fail_allocation=native_allocs+1;
    }
    return rc;
}
int main(void) {
    assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
    for(post_send_fault=0;post_send_fault<2;post_send_fault++){
        wire_written=0;heap_capacity=64000;fail_allocation=0;rx_at=rx_size=tx_size=0;
        network.mqttread=read_packet;network.mqttwrite=pressure_after_write;
        assert(ph_mqtt_client_init(&client,&network,50)==0);
        client.isconnected=1;client.keepAliveInterval=30;client.heartbeat_retry_max=1;
        countdown(&client.ping_timer,30);ph_mqtt_session_begin();
        char body[1400];memset(body,'x',sizeof(body));
        MQTTMessage m={0};m.qos=QOS1;m.payload=body;m.payloadlen=sizeof(body);
        int rc=ph_mqtt_publish(&client,"tx/state",&m);ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
        assert(wire_written>1400);
        assert(rc==MQTT_SUCCESS && rx_at==rx_size && rx_at==4);
        assert(!d.errors && !d.reconnects && d.failure.reason==PH_MQTT_CAUSE_UNKNOWN);
        assert(client.buf_size>=DEFAULT_SENDBUF_SIZE && client.buf);
        assert(ph_mqtt_yield(&client,1)==MQTT_SUCCESS);
        printf("Post-send pressure handled: rc=0, write=%u, PUBACK read=4, existing TX workspace=%zu\n",wire_written,client.buf_size);
        heap_capacity=64000;fail_allocation=0;cleanup();assert(!charged_live);
    }
}
