/* Actual native common service through the pinned Paho callback path. */
#define REAL_SERVICE 1
#define OVERLOAD_FIXTURE 1
#define main protocol_fixture_main
#include "paho.c"
#undef main
#include "common/service.c"
#include "common/mqtt/service.h"

static unsigned mutations, result_count, busy_count, large_writes;
static bool rename_target, large_receipt, pressure_receipt, pressure_inject;
static size_t last_result_length;
static char last_result_topic[512], last_result_code[32];
static char command_topic[512];

static int sender(const char *topic,const void *body,size_t length,bool retained)
{
    MQTTMessage message={0};message.qos=1;message.retained=retained;
    message.payload=(void *)body;message.payloadlen=length;
    return ph_mqtt_publish(&client,topic,&message);
}

static void real_handler(MessageData *data)
{
    (void)service_mqtt_input(sender,&account,data->message->payload,
                             data->message->payloadlen,data->message->retained);
}

static int fixture_receipt_send_bytes(const mqtt_target_t *,const void *,size_t,
                                      void *,void (*)(void *));
#define ph_mqtt_receipt_send_bytes(...) fixture_receipt_send_bytes(__VA_ARGS__)
#ifdef LEGACY_READ_CREDIT
#define service_readonly_mp(...) false
#endif
#include "common/mqtt/service.c"
#undef ph_mqtt_receipt_send_bytes
#undef service_readonly_mp

static int fixture_receipt_send_bytes(const mqtt_target_t *target,const void *wire,
                                      size_t length,void *owned,void (*release)(void *))
{
    if(pressure_inject)pressure_receipt=true;
    return ph_mqtt_receipt_send_bytes(target,wire,length,owned,release);
}

static void overload_publication(const unsigned char *topic,unsigned topic_length,
                                 const unsigned char *body,size_t length)
{
    if(topic_length<7 || memcmp(topic+topic_length-7,"/result",7))return;
    assert(topic_length<sizeof(last_result_topic));
    memcpy(last_result_topic,topic,topic_length);last_result_topic[topic_length]=0;
    last_result_length=length;result_count++;
    ph_value root,code;
    assert(phv_parse(PH_WIRE_MSGPACK,body,length,&root));
    assert(phv_get(&root,"code",&code)&&phv_text(&code,last_result_code,sizeof(last_result_code)));
    if(!strcmp(last_result_code,"busy"))busy_count++;
}

static int service_write_packet(Network *network,unsigned char *packet,int count,int timeout)
{
    if(large_receipt && (packet[0]>>4)==PUBLISH){
        const ph_mqtt_core *core=ph_mqtt_debug_core();
        assert(core->dispatch_depth==1 && core->protocol_depth==1);
        assert(count>512 && client.buf_size>512 && client.buf_size>=(size_t)count);
        assert(((allocation *)client.buf-1)->entry.size==client.buf_size);
        large_writes++;
    }
    return write_packet(network,packet,count,timeout);
}

static void overload_handler(MessageData *data){(void)data;assert(false);}

static void start_service_client(void)
{
    rx_at=rx_size=tx_size=0;network.mqttread=read_packet;network.mqttwrite=service_write_packet;
    snprintf(command_topic,sizeof(command_topic),"%s/command",account.base);
    snprintf(injected_topic,sizeof(injected_topic),"%s",command_topic);
    assert(ph_mqtt_client_init(&client,&network,50)==MQTT_SUCCESS);client.isconnected=1;
    client.keepAliveInterval=30;client.heartbeat_retry_max=1;countdown(&client.ping_timer,30);
    assert(ph_mqtt_subscribe(&client,command_topic,QOS1,handler)==MQTT_SUCCESS);
}

static void finish_service_client(void)
{
    fail_allocation=0;assert(ph_mqtt_client_deinit(&client)==MQTT_SUCCESS);
    ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
    assert(!d.bytes&&!d.credit&&!d.inflight[0]&&!d.inflight[1]&&!d.queued[0]&&!d.queued[1]);
}

static ph_buf *request_mp(const char *action,const char *id,int on)
{
    ph_writer writer;assert(phw_init(&writer,PH_WIRE_MSGPACK));
    phw_begin(&writer,NULL,false);phw_text(&writer,"action",action);
    if(id)phw_text(&writer,"request_id",id);
    phw_begin(&writer,"params",false);
    if(on>=0)phw_bool(&writer,"on",on!=0);
    phw_end(&writer);phw_end(&writer);return phw_take(&writer);
}

static void incoming_bytes(const char *topic,const void *body,size_t length,unsigned id)
{
    unsigned char packet[12000];MQTTString name=MQTTString_initializer;name.cstring=(char *)topic;
    int n=MQTTSerialize_publish(packet,sizeof(packet),0,1,0,id,name,(unsigned char *)body,length);
    assert(n>0);append(packet,(size_t)n);
}

static int command_mp(ph_buf *wire)
{
    incoming_bytes(command_topic,wire->buf,wire->len,15);
    int rc=ph_mqtt_yield(&client,1);if(rc==MQTT_SUCCESS)rc=ph_mqtt_poll(&client);return rc;
}

static void clear_receipts(void)
{
    for(unsigned i=0;i<8;i++){ph_buf_free(mp_receipts[i].response);memset(&mp_receipts[i],0,sizeof(mp_receipts[i]));}
    mp_receipt_next=0;
}

void service_product_status_mp(ph_writer *output)
{phw_begin(output,NULL,false);phw_text(output,"model","fixture");phw_end(output);}
void service_product_capabilities_mp(ph_writer *output,const char *const *extra,bool restricted)
{(void)extra;(void)restricted;phw_begin(output,NULL,false);phw_end(output);}
void service_product_diagnostics_mp(ph_writer *output){phw_begin(output,NULL,false);phw_end(output);}
void service_product_info_mp(ph_writer *output)
{phw_begin(output,NULL,false);phw_text(output,"model","fixture");phw_end(output);}
bool service_product_ota_start(const char *url){(void)url;return true;}
bool automation_mp(const char *action,const ph_value *params,ph_writer *output,const char **code)
{(void)action;(void)params;(void)output;(void)code;return false;}
static bool fixture_product(const char *action,const ph_value *params,ph_writer *output,const char **code)
{
    (void)params;
    if(strcmp(action,"fan.set")&&strcmp(action,"system.reset"))return false;
    mutations++;*code="accepted";
    if(rename_target)strcpy(account.base,"renamed");
    phw_begin(output,NULL,false);
    if(large_receipt){char padding[1537];memset(padding,'x',sizeof(padding)-1);padding[sizeof(padding)-1]=0;phw_text(output,"padding",padding);}
    phw_end(output);return true;
}
bool service_product_mp(const char *action,const ph_value *params,ph_writer *output,const char **code)
{return fixture_product(action,params,output,code);}
bool service_product_mqtt_mp(const char *action,const ph_value *params,ph_writer *output,const char **code)
{return fixture_product(action,params,output,code);}

#ifdef READ_QUERY_FIXTURE
int main(void)
{
    service_init();assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));heap_capacity=64000;
    start_service_client();
    ph_buf *read=request_mp("status.get","small-read",-1);
    ph_buf *write=request_mp("fan.set","protected-write",1);
    ph_buf *unknown=request_mp("future.get","future",-1);
    assert(service_readonly_mp(read->buf,read->len));
    assert(!service_readonly_mp(write->buf,write->len));
    assert(!service_readonly_mp(unknown->buf,unknown->len));
    assert(!service_readonly_mp(read->buf,read->len-1));
    /* The backlog fits both requests and their small reply, but not the old
     * unconditional 8 KiB write-receipt reservation. Execute actual Paho
     * callback/owner paths, including a freed borrowed request under ASan. */
    char padding[7000];memset(padding,'x',sizeof(padding));
    ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
    phw_text(&w,"action","future.get");phw_begin(&w,"params",false);
    phw_text_n(&w,"padding",padding,sizeof(padding));phw_end(&w);phw_end(&w);
    ph_buf *backlog=phw_take(&w);assert(backlog);
    incoming_bytes(command_topic,read->buf,read->len,41);
    incoming_bytes(command_topic,write->buf,write->len,42);
    incoming_bytes(command_topic,backlog->buf,backlog->len,43);
    assert(ph_mqtt_yield(&client,1)==MQTT_SUCCESS);
    assert(ph_mqtt_poll(&client)==MQTT_SUCCESS);
    assert(result_count==1&&!strcmp(last_result_code,"ok")&&!mutations&&!busy_count);
    /* The backlog remains. Mutations still require the full result
     * reservation before execution; overload cannot execute a hidden write. */
    assert(ph_mqtt_poll(&client)==MQTT_SUCCESS);
    assert(result_count==2&&!strcmp(last_result_code,"busy")&&!mutations&&busy_count==1);
    assert(!ph_mqtt_debug_core()->credit&&!ph_mqtt_receipt_pending());
    assert(ph_mqtt_poll(&client)==MQTT_SUCCESS); /* Drain the unknown read. */
    /* Once drained, that same write executes once, then replays its receipt. */
    assert(command_mp(write)==MQTT_SUCCESS&&mutations==1);
    assert(command_mp(write)==MQTT_SUCCESS&&mutations==1);
    ph_buf_free(read);ph_buf_free(write);ph_buf_free(unknown);ph_buf_free(backlog);
    finish_service_client();clear_receipts();assert(mp_cache_evict());assert(!native_live&&!charged_live);
    ph_mqtt_owner_end();puts("native service read actual-size credit, borrowed lifetime, write pre-admission/replay PASS");return 0;
}
#elif defined(PREFLIGHT_FIXTURE)
#include "preflight_service_mp.in"
#elif defined(LARGE_RECEIPT_FIXTURE)
int main(void)
{
    service_init();assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));heap_capacity=64000;
    start_service_client();assert(client.buf_size==512);large_receipt=true;
    ph_buf *request=request_mp("fan.set","large-result",1);assert(command_mp(request)==MQTT_SUCCESS);
    assert(mutations==1&&large_writes==1&&last_result_length>1024);
    assert(client.buf_size==512&&((allocation *)client.buf-1)->entry.size==512);
    assert(result_count==1&&!strcmp(last_result_code,"accepted"));
    assert(!ph_mqtt_receipt_pending()&&!ph_mqtt_debug_core()->credit);
    assert(command_mp(request)==MQTT_SUCCESS&&mutations==1&&large_writes==2&&result_count==2);
    ph_buf_free(request);finish_service_client();clear_receipts();assert(!native_live&&!charged_live);
    ph_mqtt_owner_end();puts("native service receipt >1KiB: Paho TX grow/shrink and replay PASS");return 0;
}
#else
int main(void)
{
    service_init();assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));heap_capacity=64000;
    start_service_client();rename_target=true;
    char original_target[512];snprintf(original_target,sizeof(original_target),"%s/result",account.base);
    ph_buf *same=request_mp("fan.set","rename-once",1);assert(command_mp(same)==MQTT_SUCCESS);
    assert(mutations==1&&!strcmp(last_result_topic,original_target)&&!strcmp(last_result_code,"accepted"));
    assert(command_mp(same)==MQTT_SUCCESS&&mutations==1&&!strcmp(last_result_code,"accepted"));
    ph_buf *conflict=request_mp("fan.set","rename-once",0);assert(command_mp(conflict)==MQTT_SUCCESS);
    assert(mutations==1&&!strcmp(last_result_code,"conflict"));
    ph_buf_free(same);ph_buf_free(conflict);finish_service_client();clear_receipts();

    rename_target=false;assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    start_service_client();unsigned before=mutations,busy_before=busy_count;
    ph_buf *queued[6];for(unsigned i=0;i<6;i++){char id[24];snprintf(id,sizeof(id),"queue-%u",i);queued[i]=request_mp("fan.set",id,1);incoming_bytes(command_topic,queued[i]->buf,queued[i]->len,100+i);}
    assert(ph_mqtt_yield(&client,1)==MQTT_SUCCESS&&mutations==before);
    ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);assert(d.queued[PH_MQTT_COMMAND]==4);
    for(unsigned i=0;i<8;i++)assert(ph_mqtt_poll(&client)==MQTT_SUCCESS);
    assert(mutations==before+4&&busy_count==busy_before+2);
    for(unsigned i=0;i<6;i++)ph_buf_free(queued[i]);finish_service_client();clear_receipts();

    start_service_client();before=mutations;
    ph_buf *anonymous=request_mp("fan.set",NULL,1);
    for(unsigned i=0;i<7;i++)incoming_bytes(command_topic,anonymous->buf,anonymous->len,200+i);
    assert(ph_mqtt_yield(&client,1)==MQTT_SUCCESS&&mutations==before);
    ph_mqtt_snapshot(&d);assert(d.queued[PH_MQTT_COMMAND]==4);
    for(unsigned i=0;i<4;i++)assert(ph_mqtt_poll(&client)==MQTT_SUCCESS);
    assert(mutations==before+4);ph_buf_free(anonymous);finish_service_client();clear_receipts();

    unsigned exercised=0;
    clear_receipts();mutations=0;assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    start_service_client();ph_buf *measure=request_mp("fan.set","allocation-measure",1);
    unsigned allocation_start=native_allocs;assert(command_mp(measure)==MQTT_SUCCESS);
    unsigned allocation_points=native_allocs-allocation_start;assert(mutations==1&&allocation_points);
    ph_buf_free(measure);finish_service_client();clear_receipts();
    for(unsigned point=1;point<=allocation_points;point++){
        clear_receipts();mutations=0;assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
        start_service_client();ph_buf *retry=request_mp("fan.set","allocation-retry",1);unsigned allocation_start=native_allocs;
        fail_allocation=allocation_start+point;(void)command_mp(retry);bool ran=mutations!=0;
        exercised+=native_allocs>=fail_allocation;finish_service_client();
        assert(mutations==(ran?1u:0u));fail_allocation=0;start_service_client();
        assert(command_mp(retry)==MQTT_SUCCESS);assert(mutations==(ran?1u:1u));
        ph_buf_free(retry);finish_service_client();clear_receipts();assert(!native_live&&!charged_live);
    }
    assert(exercised==allocation_points);ph_mqtt_owner_end();
    printf("native common service/Paho: rename target, ID replay/conflict, busy+IDless queue, %u allocation failures PASS\n",exercised);
    return 0;
}
#endif
