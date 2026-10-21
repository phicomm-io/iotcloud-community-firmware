/* Actual production owner and pinned Paho, two independent broker byte streams. */
#define main primary_fixture_main
#include "paho.c"
#undef main
#include "bemfa_transport.h"
int mico_rtos_deinit_mutex(mico_mutex_t *m){assert(!*m);return 0;}
typedef struct {unsigned char input[65536];size_t at,size;} peer;
static peer peers[2];static unsigned active;
static Client second;static Network second_network;static mqtt_account_t second_account;
static mico_thread_t second_thread=2;static unsigned calls[2];
static void select_peer(unsigned i) {
    peers[active].at=rx_at;peers[active].size=rx_size;memcpy(peers[active].input,rx,rx_size);
    active=i;rx_at=peers[i].at;rx_size=peers[i].size;memcpy(rx,peers[i].input,rx_size);
    assert(!tx_size);current_thread=i+1;
}
static bool admit(MessageData *d,messageHandler h){(void)h;return !d->message->retained;}
static void dual_handler(MessageData *d) {
    const ph_mqtt_core *core=active?bm_debug_core():ph_mqtt_debug_core();
    assert(!core->protocol_depth && core->dispatch_depth==1);assert(!d->message->retained);
    calls[active]++;
    if(active) {
        /* An existing feature producer called by Bemfa still targets primary. */
        assert(ph_mqtt_enqueue_send("primary/state","state",5,1,false)==0);
        assert(ph_mqtt_debug_core()->count[PH_MQTT_SEND]==1);
        assert(!bm_debug_core()->count[PH_MQTT_SEND]);
        assert(ph_mqtt_publish(&second,"wrong/owner",d->message)!=0);
    }
}
static void connect_peer(unsigned i) {
    Client *c=i?&second:&client;Network *n=i?&second_network:&network;
    assert(NewNetwork(n,"fixture",1883,(ssl_opts){0})==0);
    assert((i?bm_client_init(c,n,50):ph_mqtt_client_init(c,n,50))==0);
    MQTTPacket_connectData data=MQTTPacket_connectData_initializer;
    data.MQTTVersion=4;data.clientID.cstring=i?"phicomm-tc1-bemfa":"primary-device";
    data.username.cstring=i?"app-id":"primary-user";data.password.cstring=i?"secret-key":"primary-password";
    assert((i?bm_connect(c,&data):ph_mqtt_connect(c,&data))==0);
}
int main(void) {
    heap_capacity=120000;assert(mqtt_account_init(&account,1,"tc1","user","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,admit));connect_peer(0);
    assert(ph_mqtt_subscribe(&client,"primary/command",QOS1,dual_handler)==0);
    assert(publish()==0);size_t baseline=native_live;unsigned initial=native_allocs;
    static char topics[6][24];for(unsigned i=0;i<6;i++)snprintf(topics[i],sizeof(topics[i]),"outlet%u001",i+1);
    for(unsigned round=0;round<100;round++) {
        select_peer(1);memset(&peers[1],0,sizeof(peers[1]));rx_at=rx_size=0;
        fail_allocation=native_allocs+1;assert(!bm_transport_allocate(&second_thread));fail_allocation=0;
        /* Failed initialization must also release the dynamic context. */
        assert(bm_transport_allocate(&second_thread));assert(!bm_owner_init(&second_account,0,&second_thread,admit));
        bm_transport_release();assert(native_live==baseline);
        assert(bm_transport_allocate(&second_thread));assert(bm_owner_init(NULL,16,&second_thread,admit));connect_peer(1);
        for(unsigned i=0;i<6;i++)assert(bm_subscribe(&second,topics[i],QOS1,dual_handler)==0);
        unsigned before=calls[1];for(unsigned i=0;i<6;i++)incoming(topics[i],"on",i+1);
        for(unsigned j=0;j<6;j++)assert(bm_yield(&second,10)==0);assert(calls[1]==before); /* callback only queues */
        for(unsigned i=0;i<6;i++) {
            assert(bm_poll(&second)==0);
            select_peer(0);assert(ph_mqtt_poll(&client)==0);select_peer(1);
        }
        /* Queue capacity is four; remaining commands must be visible drops. */
        assert(calls[1]==before+PH_MQTT_COMMAND_COUNT);
        assert(bm_debug_core()->ingress_dropped>=2);
        unsigned before_primary=calls[0];select_peer(0);incoming("primary/command","on",51);
        assert(ph_mqtt_yield(&client,10)==0);assert(ph_mqtt_poll(&client)==0);assert(calls[0]==before_primary+1);
        select_peer(1);MQTTMessage message={0};message.qos=QOS1;message.payload="on";message.payloadlen=2;
        fail_send=true;assert(bm_publish(&second,"outlet1001/up",&message)!=0);fail_send=false;
        bm_pending_clear();assert(bm_client_deinit(&second)==0);second_network.disconnect(&second_network);bm_transport_release();
        assert(native_live==baseline);
        select_peer(0);assert(ph_mqtt_owner());assert(publish()==0); /* other connection survives */
    }
    assert(native_allocs>initial);ph_mqtt_client_deinit(&client);ph_mqtt_owner_end();
    assert(!native_live && !charged_live);
    puts("dual production owners + pinned Paho: independent streams, six subscriptions, overload, primary producer routing, secondary failures and 100 leak-free restarts PASS");
}
