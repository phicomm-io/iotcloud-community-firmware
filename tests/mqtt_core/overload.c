/* In-memory actual pinned Paho: bounded ingress refusal, never DUT proof. */
#define OVERLOAD_FIXTURE
#define main existing_paho_scenarios
#include "paho.c"
#undef main
#include "../../common/busy_reply.h"
static unsigned busy_sent,generic_busy_sent;
static size_t expected_large;
static char received[128],modern_topic[512],result_topic[512];
static void overload_handler(MessageData *d) {
    assert(!ph_mqtt_debug_core()->protocol_depth && ph_mqtt_debug_core()->dispatch_depth==1);
    executions++;
    if(expected_large && d->message->payloadlen==expected_large) {
        assert(((char *)d->message->payload)[0]=='x');
        return;
    }
    assert(d->message->payloadlen<sizeof(received));
    memcpy(received,d->message->payload,d->message->payloadlen);received[d->message->payloadlen]=0;
}
static void overload_publication(const unsigned char *topic,unsigned tn,const unsigned char *body,size_t n) {
    if(n>=6 && memmem(body,n,"\"busy\"",6)) {
        assert(tn==strlen(result_topic) && !memcmp(topic,result_topic,tn));
        if(memmem(body,n,"\"request_id\":",13))
            assert(memmem(body,n,"\"request_id\":\"paho-1\"",21));
        else {assert(n==strlen("{\"code\":\"busy\",\"data\":{}}"));generic_busy_sent++;}
        busy_sent++;
    }
}
static ph_mqtt_diagnostics snap(void) { ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);return d; }
static void fresh(void) {
    heap_capacity=40000;fail_send=missing_ack=false;fail_allocation=0;
    init();ph_mqtt_session_begin();
    assert(!ph_mqtt_subscribe(&client,modern_topic,QOS1,handler));
}
static void unchanged_connection(ph_mqtt_diagnostics before,unsigned drops,unsigned busy) {
    ph_mqtt_diagnostics d=snap();
    assert(d.ingress_dropped==before.ingress_dropped+drops);
    assert(d.busy_overflow==before.busy_overflow+busy);
    assert(d.errors==before.errors && d.generation==before.generation);
    assert(!memcmp(&d.failure,&before.failure,sizeof(d.failure)) && client.isconnected);
}
static void drain(unsigned expected) {
    unsigned before=executions;
    for(unsigned i=0;i<expected;i++)assert(!ph_mqtt_poll(&client));
    assert(executions==before+expected && !ph_mqtt_work_pending());
    assert(!ph_mqtt_debug_core()->bytes && !ph_mqtt_debug_core()->credit);
}
static void retained(const char *topic) {
    unsigned char wire[1024];MQTTString t=MQTTString_initializer;t.cstring=(char*)topic;
    int n=MQTTSerialize_publish(wire,sizeof(wire),0,1,1,600,t,(unsigned char*)"{}",2);
    assert(n>0);append(wire,n);
}
int main(void) {
    assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(mqtt_leaf(&account,"command",modern_topic,sizeof(modern_topic)));
    assert(mqtt_leaf(&account,"result",result_topic,sizeof(result_topic)));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
    fresh();ph_mqtt_diagnostics before=snap();unsigned ran=executions;
    inject_count=5;assert(!publish());unchanged_connection(before,1,0);
    assert(executions==ran && snap().queued[0]==4);drain(4);
    incoming("rx/command","latest",901);assert(!ph_mqtt_yield(&client,1));drain(1);
    assert(!strcmp(received,"latest"));cleanup();

    /* Actual PUBACK callbacks may legally consume the entire ledger. Neither
     * saturation nor a refused retained entry runs business or drops the socket. */
    unsigned busy;char saturated_body[8193],external[4096];
    for(unsigned retained_first=0;retained_first<2;retained_first++) {
        fresh();before=snap();ran=executions;busy=busy_sent;
        expected_large=6144-sizeof(ph_mqtt_item)-strlen("rx/command")-2-16;
        memset(saturated_body,'x',expected_large);saturated_body[expected_large]=0;
        MQTTString t=MQTTString_initializer;t.cstring="rx/command";
        unsigned char wire[12000];
        int n=MQTTSerialize_publish(wire,sizeof(wire),0,1,retained_first,910,t,
                                   (unsigned char*)saturated_body,expected_large);
        assert(n>0);append(wire,n);incoming("rx/command",saturated_body,911);
        assert(!publish());
        assert(snap().bytes==PH_MQTT_QUEUE_BYTES && snap().queued[0]==2 && !snap().credit);
        assert(!ph_mqtt_poll(&client));unchanged_connection(before,retained_first?0:1,0);
        assert(executions==ran && busy_sent==busy && snap().queued[0]==1 && !snap().credit);
        drain(1);expected_large=0;
        incoming("rx/command","capacity recovered",912);assert(!publish());drain(1);
        assert(!strcmp(received,"capacity recovered"));cleanup();
    }

    /* Repeated callback refill plus an external producer must still send one
     * queued SEND on each refused COMMAND turn, including a modern busy turn. */
    for(unsigned variant=0;variant<4;variant++) {
        bool modern_request=variant==1 || variant==3,fail_transport=variant>=2;
        fresh();ran=executions;
        const char *input=modern_request?modern_topic:"rx/command";
        expected_large=4096-sizeof(ph_mqtt_item)-strlen(input)-2-16;
        memset(saturated_body,'x',expected_large);saturated_body[expected_large]=0;
        size_t external_n=4096-sizeof(ph_mqtt_item)-strlen("tx/state")-2-16;
        memset(external,'y',external_n);external[external_n]=0;
        incoming(input,saturated_body,914);
        for(unsigned round=0;round<3;round++) {
            before=snap();busy=busy_sent;
            current_thread=2;
            assert(!ph_mqtt_enqueue_send("tx/state",external,external_n,QOS1,false));
            current_thread=1;
            incoming(input,saturated_body,915+round);assert(!publish());
            assert(snap().bytes==PH_MQTT_QUEUE_BYTES && snap().queued[0]==2 && snap().queued[1]==1);
            unsigned sent=publications;
            if(fail_transport) {
                fail_send=true;assert(ph_mqtt_poll(&client)!=MQTT_SUCCESS);
                ph_mqtt_diagnostics failed=snap();
                assert(failed.errors>before.errors && failed.failure.reason==PH_MQTT_CAUSE_WRITE);
                assert(failed.ingress_dropped==before.ingress_dropped+(modern_request?0:1) && !failed.busy_overflow);
                assert(executions==ran && busy_sent==busy && failed.queued[1]==(modern_request?1:0));
                fail_send=false;break;
            }
            assert(!ph_mqtt_poll(&client));unchanged_connection(before,modern_request?0:1,0);
            assert(executions==ran && busy_sent==busy+modern_request);
            assert(publications==sent+1+modern_request && snap().queued[1]==0 && snap().queued[0]==1);
        }
        if(!fail_transport)drain(1);
        expected_large=0;cleanup();
    }

    /* Existing credit is not normal saturation: preserve the fatal invariant
     * path and never execute a command under somebody else's receipt credit. */
    fresh();ran=executions;
    incoming("rx/command","must not execute",913);assert(!ph_mqtt_yield(&client,1));
    assert(ph_mqtt_credit_acquire((ph_mqtt_core *)ph_mqtt_debug_core(),SERVICE_BUSY_BYTES));
    before=snap();assert(ph_mqtt_poll(&client)==MQTT_FAILURE);
    assert(executions==ran && snap().ingress_dropped==before.ingress_dropped);
    assert(snap().operation==PH_MQTT_ERROR_APP_RECEIPT_ADMISSION);
    cleanup();

    fresh();strcpy(injected_topic,modern_topic);before=snap();busy=busy_sent;ran=executions;
    inject_count=7;assert(!publish());unchanged_connection(before,1,1);
    assert(executions==ran && snap().queued[0]==4);drain(4);assert(busy_sent==busy+2);cleanup();

    /* Retained refusals never execute or need an application failure receipt. */
    fresh();before=snap();busy=busy_sent;inject_count=4;assert(!publish());
    retained(modern_topic);assert(!publish());unchanged_connection(before,0,0);
    drain(4);assert(busy_sent==busy);cleanup();

    /* An oversize business payload fits the protocol frame guard but not ingress. */
    fresh();before=snap();unsigned allocations=native_allocs;
    char large[8194];memset(large,' ',sizeof(large)-1);large[sizeof(large)-1]=0;
    incoming(modern_topic,large,902);assert(!ph_mqtt_yield(&client,1));
    unchanged_connection(before,0,0);assert(!snap().queued[0]);
    /* No queued command/allocation is retained; the ID-less busy is safe. */
    assert(native_allocs>=allocations);busy=busy_sent;
    unsigned generic=generic_busy_sent;ran=executions;
    assert(!ph_mqtt_poll(&client));assert(busy_sent==busy+1 && generic_busy_sent==generic+1);
    assert(executions==ran && !ph_mqtt_work_pending());cleanup();

    /* Reserve refusal and allocator failure remain bounded and recoverable. */
    for(unsigned mode=0;mode<2;mode++) {
        fresh();before=snap();busy=busy_sent;allocations=native_allocs;
        if(!mode)heap_capacity=charged_live+16384+32;
        else fail_allocation=native_allocs+1;
        incoming(modern_topic,"{\"request_id\":\"paho-1\"}",903+mode);
        assert(!ph_mqtt_yield(&client,1));unchanged_connection(before,0,0);
        assert(!snap().queued[0]);
        if(!mode)assert(native_allocs==allocations);
        else assert(native_allocs==allocations+1);
        heap_capacity=40000;fail_allocation=0;assert(!ph_mqtt_poll(&client));
        assert(busy_sent==busy+1 && !ph_mqtt_work_pending());cleanup();
    }

    /* During the first busy PUBACK wait the just-freed slot can refill. The
     * second old slot remains occupied; two more messages visibly drop. */
    fresh();before=snap();busy=busy_sent;ran=executions;
    inject_count=6;assert(!publish());inject_count=3;
    assert(!ph_mqtt_poll(&client));unchanged_connection(before,2,2);
    assert(busy_sent==busy+2 && executions==ran+1 && ph_mqtt_work_pending());
    drain(3);assert(busy_sent==busy+3);cleanup();

    /* Genuine SDK transport failure is still fatal after healthy overload. */
    fresh();before=snap();inject_count=7;assert(!publish());unchanged_connection(before,1,1);
    fail_send=true;assert(publish()!=0);ph_mqtt_diagnostics failed=snap();
    assert(failed.errors>before.errors && failed.failure.reason==PH_MQTT_CAUSE_WRITE);
    assert(ph_mqtt_poll(&client)!=0);fail_send=false;cleanup();
    ph_mqtt_owner_end();assert(!native_live&&!charged_live);
    puts("host bounded overload: ingress/receipt-capacity/retained/refill-SEND/invariant/SDK-fault/recovery PASS");
}
