/* Actual shared client/Paho, in-memory fault injection; no device evidence. */
#define main existing_paho_scenarios
#include "paho.c"
#undef main
#include "diagnostics.h"
static int broken_read(Network *n, unsigned char *p, int count, int timeout) {
    (void)n; (void)p; (void)count; (void)timeout; now_ms += 7; return -1;
}
static int malformed_read(Network *n, unsigned char *p, int count, int timeout) {
    (void)n; (void)timeout; assert(count == 1); *p = 0; return 1;
}
static int short_body_read(Network *n, unsigned char *p, int count, int timeout) {
    return read_packet(n,p,count>2?count-1:count,timeout);
}
static void fresh(void) { heap_capacity=40000; fail_send=missing_ack=false; init(); ph_mqtt_session_begin(); }
static ph_mqtt_diagnostics evidence(unsigned reason, unsigned op) {
    ph_mqtt_diagnostics d; ph_mqtt_snapshot(&d);
    assert(d.failure.reason==reason && d.failure.operation==op);
    assert(d.last_stage==op && d.last_error<0);
    return d;
}
int main(void) {
    assert(sizeof(ph_mqtt_failure_evidence)==60);
    assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
    fresh(); ph_mqtt_diagnostics mark_before,mark_after;
    ph_mqtt_snapshot(&mark_before);
    assert(ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_GRAPH,-1)==-1);
    ph_mqtt_snapshot(&mark_after);
    assert(mark_after.errors==mark_before.errors && mark_after.operation==8);
    ph_mqtt_error(PH_MQTT_ERROR_SESSION,-1);
    evidence(PH_MQTT_CAUSE_UNKNOWN,PH_MQTT_ERROR_APP_DISCOVERY_GRAPH);
    ph_mqtt_session_begin();
    ph_mqtt_snapshot(&mark_after); assert(mark_after.operation==0);
    ph_mqtt_error(PH_MQTT_ERROR_SESSION,-1);
    evidence(PH_MQTT_CAUSE_UNKNOWN,PH_MQTT_ERROR_SESSION);
    ph_mqtt_session_begin();
    assert(ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_GRAPH,0)==0);
    assert(ph_mqtt_app_error(99,-1)==-1);
    ph_mqtt_snapshot(&mark_after); assert(mark_after.operation==0);
    assert(ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_ENCODING,-1)==-1);
    assert(publish()==0); /* A successful SDK operation clears the transient tag. */
    ph_mqtt_error(PH_MQTT_ERROR_SESSION,-1);
    evidence(PH_MQTT_CAUSE_UNKNOWN,PH_MQTT_ERROR_SESSION); cleanup();
    fresh(); unsigned allocations=native_allocs, sent=publications;
    heap_capacity=charged_live+16384+40;
    assert(publish()==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);
    now_ms+=30000;
    assert(publish()==MQTT_FAILURE);
    ph_mqtt_diagnostics denied=evidence(PH_MQTT_CAUSE_RESOURCE_EXPIRED,PH_MQTT_ERROR_PUBLISH);
    assert(denied.failure.packet_bytes>0 && denied.failure.free_bytes==16384+40);
    assert(native_allocs==allocations && sent==publications);
    assert(!denied.failure.read.requested && !denied.failure.write.requested);
    heap_capacity=40000; cleanup();
    ph_mqtt_diagnostics persisted;ph_mqtt_snapshot(&persisted);
    assert(!memcmp(&persisted.failure,&denied.failure,sizeof(denied.failure)));
    fresh(); assert(publish()==0);ph_mqtt_snapshot(&persisted);
    assert(!memcmp(&persisted.failure,&denied.failure,sizeof(denied.failure)));
    fail_send=true; assert(publish()!=0);
    ph_mqtt_diagnostics write=evidence(PH_MQTT_CAUSE_WRITE,PH_MQTT_ERROR_PUBLISH);
    assert(write.failure.write.returned==-1 && write.failure.write.elapsed_ms==1);
    /* Teardown/secondary errors must not replace the true failing operation. */
    ph_mqtt_app_error(PH_MQTT_ERROR_APP_OTA_GRAPH,-1);
    ph_mqtt_error(PH_MQTT_ERROR_SESSION,-1);cleanup();ph_mqtt_snapshot(&persisted);
    assert(!memcmp(&persisted.failure,&write.failure,sizeof(write.failure)));
    fresh(); /* Rebind only the original read adapter, preserving wrappers. */
    cleanup();network.mqttread=broken_read;network.mqttwrite=write_packet;
    assert(ph_mqtt_client_init(&client,&network,50)==0);client.isconnected=1;
    client.keepAliveInterval=30;client.heartbeat_retry_max=1;countdown(&client.ping_timer,30);
    ph_mqtt_session_begin(); assert(ph_mqtt_yield(&client,10)!=0);
    ph_mqtt_diagnostics read=evidence(PH_MQTT_CAUSE_READ,PH_MQTT_ERROR_YIELD);
    assert(read.failure.read.requested==1 && read.failure.read.returned==-1 && read.failure.read.elapsed_ms==7);
    cleanup();assert(network.mqttread==broken_read && network.mqttwrite==write_packet);
    network.mqttread=malformed_read;network.mqttwrite=write_packet;
    assert(ph_mqtt_client_init(&client,&network,50)==0);client.isconnected=1;
    client.keepAliveInterval=30;client.heartbeat_retry_max=1;countdown(&client.ping_timer,30);
    ph_mqtt_session_begin();assert(ph_mqtt_yield(&client,10)!=0);
    evidence(PH_MQTT_CAUSE_RX_FRAMING,PH_MQTT_ERROR_YIELD);cleanup();
    fresh();missing_ack=true;assert(publish()!=0);
    ph_mqtt_diagnostics sdk=evidence(PH_MQTT_CAUSE_SDK,PH_MQTT_ERROR_PUBLISH);
    assert(sdk.failure.read.returned==0 && sdk.failure.read.timeout_ms>0);
    cleanup();
    /* This is a nonmodern queue overflow, not emergency-slot exhaustion. */
    fresh();ph_mqtt_diagnostics before;ph_mqtt_snapshot(&before);
    inject_count=5;assert(publish()==0);ph_mqtt_snapshot(&persisted);
    assert(persisted.ingress_dropped==before.ingress_dropped+1);
    assert(persisted.busy_overflow==before.busy_overflow && persisted.errors==before.errors);
    assert(!memcmp(&persisted.failure,&before.failure,sizeof(before.failure)));
    cleanup();
    network.mqttread=short_body_read;network.mqttwrite=write_packet;
    assert(ph_mqtt_client_init(&client,&network,50)==0);client.isconnected=1;
    client.keepAliveInterval=30;client.heartbeat_retry_max=1;countdown(&client.ping_timer,30);
    ph_mqtt_session_begin();incoming("rx/command","{}",71);assert(ph_mqtt_yield(&client,10)!=0);
    ph_mqtt_diagnostics short_read=evidence(PH_MQTT_CAUSE_READ,PH_MQTT_ERROR_YIELD);
    assert(short_read.failure.read.returned==short_read.failure.read.requested-1 && short_read.failure.rx_state==2);
    cleanup();
    fresh();write_fragment=2;assert(publish()==0);cleanup();write_fragment=65535;
    ph_mqtt_session_begin();
    ph_mqtt_app_error(PH_MQTT_ERROR_APP_RECEIPT_ADMISSION,-1);
    ph_mqtt_error(PH_MQTT_ERROR_SESSION,-1);
    evidence(PH_MQTT_CAUSE_UNKNOWN,14);
    char wire[2304]="{\"data\":{}}";
    size_t used=ph_mqtt_diag_append(wire,sizeof(wire),strlen(wire),true);
    assert(used>0 && strstr(wire,"\"operation\":14") && strstr(wire,"\"mqtt_failure\":{") && strstr(wire,"\"read\":["));
    puts("host first-failure evidence: admission/write/read/framing/SDK/lifecycle PASS");
}
