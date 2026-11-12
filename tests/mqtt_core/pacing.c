/* Real shared owner/session and pinned Paho; deterministic in-memory time. */
#define main existing_paho_scenarios
#define mico_thread_msleep fixture_sleep
#include "paho.c"
#undef mico_thread_msleep
#undef main

static unsigned pace_mode, pace_cycles, pace_closed, sleeps[16], sleep_count;
static uint32_t cycle_at[16];
void mico_thread_msleep(unsigned ms) {
    assert(ms > 0);
    if (pace_cycles && !pace_closed) {
        assert(sleep_count < 16);
        sleeps[sleep_count++] = ms;
    }
    fixture_sleep(ms);
}
static int pace_prepare(void *ctx, ph_mqtt_connection *spec) {
    (void)ctx;
    rx_at = rx_size = tx_size = 0;
    *spec = (ph_mqtt_connection){&account,"broker","user","password",
        "device","tx/availability",1883,50,NULL,"test","full"};
    return PH_MQTT_PREPARE_READY;
}
static int pace_initial(void *ctx) { (void)ctx; return MQTT_SUCCESS; }
static int pace_cycle(void *ctx) {
    (void)ctx;
    assert(pace_cycles < 16);
    cycle_at[pace_cycles++] = now_ms;
    if (pace_mode == 1 && pace_cycles == 1) {
        for (unsigned i = 0; i < 4; ++i)
            assert(ph_mqtt_enqueue_send("tx/backlog","work",4,1,false) == 0);
    }
    if ((pace_mode == 2 && pace_cycles <= 2) || pace_mode == 6) {
        unsigned sent = publications;
        for (unsigned i = 0; i < 4; ++i) assert(publish() == MQTT_SUCCESS);
        assert(publish() == PH_MQTT_MORE && publications == sent+4);
        return PH_MQTT_MORE;
    }
    if (pace_mode == 4) return PH_MQTT_RECONFIGURE;
    if (pace_mode == 5) return MQTT_FAILURE;
    return MQTT_SUCCESS;
}
static bool pace_ready(void *ctx) { (void)ctx; return pace_mode != 3 || pace_cycles >= 3; }
static void pace_closing(void *ctx, bool connected, int rc) {
    (void)ctx; assert(connected);
    assert(rc == (pace_mode == 4 ? PH_MQTT_RECONFIGURE : pace_mode == 5 ? MQTT_FAILURE : MQTT_SUCCESS));
    ++pace_closed;
    assert(!ph_mqtt_work_pending());
}
static bool pace_stop(void *ctx) { (void)ctx; return pace_closed || (pace_cycles >= 4 && now_ms != cycle_at[3]); }
static void run_pace(unsigned mode) {
    pace_mode=mode; pace_cycles=pace_closed=sleep_count=0;
    ph_mqtt_session_hooks h={NULL,pace_prepare,subscription_plan,pace_initial,
        pace_cycle,pace_closing,NULL,pace_stop,pace_ready};
    unsigned sent=publications;
    ph_mqtt_session_run(&client,&h);
    assert(pace_closed==1 && !ph_mqtt_work_pending() && !native_live && !charged_live);
    if (mode == 4 || mode == 5) { assert(pace_cycles==1 && !sleep_count); return; }
    assert(pace_cycles==4 && sleep_count==4);
    if (mode==6) {
        for (unsigned i=0;i<4;++i) assert(sleeps[i]==10);
        for (unsigned i=1;i<4;++i) assert(cycle_at[i]-cycle_at[i-1]==20);
        assert(publications==sent+19); /* Sustained turns never exceed four. */
    } else if (mode==0) {
        for (unsigned i=0;i<4;++i) assert(sleeps[i]==50);
        for (unsigned i=1;i<4;++i) assert(cycle_at[i]-cycle_at[i-1]==150);
    } else {
        assert(sleeps[0]==10 && sleeps[1]==10 && sleeps[2]==50 && sleeps[3]==50);
        assert(cycle_at[1]-cycle_at[0]==20 && cycle_at[2]-cycle_at[1]==20);
        assert(cycle_at[3]-cycle_at[2]==150);
        if (mode==1) assert(publications==sent+7); /* Four SENDs, announcement, online/offline. */
        if (mode==2) assert(publications==sent+11); /* Exactly four per turn. */
    }
}
int main(void) {
    assert(mqtt_account_init(&account,1,"a1","test","aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
    for(unsigned mode=0;mode<7;++mode) run_pace(mode);
    strcpy(account.base,"rx");
    init();
    for(unsigned i=0;i<6;++i) incoming("rx/command","{}",100+i);
    for(unsigned i=0;i<6;++i) assert(ph_mqtt_yield(&client,10)==0);
    assert(ph_mqtt_work_pending());
    ph_mqtt_diagnostics d; ph_mqtt_snapshot(&d);
    assert(d.queued[PH_MQTT_COMMAND]==4);
    /* Remove ordinary work directly to isolate the owner emergency slots. */
    assert(ph_mqtt_clear((ph_mqtt_core *)ph_mqtt_debug_core()));
    assert(ph_mqtt_work_pending());
    current_thread=2; assert(!ph_mqtt_work_pending()); current_thread=1;
    ph_mqtt_pending_clear(); assert(!ph_mqtt_work_pending());
    cleanup(); ph_mqtt_owner_end(); assert(!ph_mqtt_work_pending());
    puts("owner pacing: idle/backlog/quota/initial/emergency/cleanup PASS");
}
