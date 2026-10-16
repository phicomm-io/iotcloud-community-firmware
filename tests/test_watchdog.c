#include <assert.h>
#include <stdio.h>
#include "../common/watchdog.c"
#include "../common/watchdog_flash.h"

static uint32_t now_ms, last_feed;
static unsigned feeds, critical, erases, reads;
static int reset_bits, init_error, thread_error, erase_error;
static bool business_runs;
uint32_t mico_rtos_get_time(void) { return now_ms; }
void mico_rtos_enter_critical(void) { assert(!critical); critical++; }
void mico_rtos_exit_critical(void) { assert(critical == 1); critical--; }
int get_last_reset_reason(void) { return reset_bits; }
int MicoWdgInitialize(uint32_t ms) { assert(ms == 3000); return init_error; }
void MicoWdgReload(void) { assert(critical == 1); feeds++; last_feed = now_ms; }
int mico_rtos_create_thread(void *t, int priority, const char *name,
    void (*entry)(mico_thread_arg_t), unsigned stack, int arg)
{
    (void)t; (void)arg;
    assert(priority == 0 && !strcmp(name, "watchdog") && entry == monitor && stack >= 1024);
    return thread_error;
}
void service_event(const char *level, const char *module, const char *event)
{ (void)level; assert(!strcmp(module,"system")); assert(!strcmp(event, reset_name())); }
static void beat_all(void)
{
    for (unsigned i = 0; i < WATCHDOG_OTA; i++) watchdog_beat(i);
}
void mico_thread_msleep(unsigned ms)
{
    assert(!critical); now_ms += ms;
    if (business_runs) { beat_all(); watchdog_poll(); }
}
int MicoFlashErase(int partition, uint32_t offset, uint32_t size)
{
    assert(!critical && partition == MICO_PARTITION_OTA_TEMP);
    assert(offset == erases * 4096 && size == 4096);
    erases++; now_ms += 100;
    return erase_error && erases == (unsigned)erase_error ? -1 : 0;
}
int MicoFlashRead(int partition, uint32_t *offset, void *data, uint32_t size)
{
    assert(!critical && partition == MICO_PARTITION_OTA_TEMP);
    memset(data, 0x55, size); *offset += size; reads++; now_ms += 5; return 0;
}
static void boot(uint32_t time, int reason)
{
    started = enabled = startup_complete = failed = false; fault = "";
    now_ms = time; reset_bits = reason;
    init_error = thread_error = erase_error = 0;
    feeds = erases = reads = critical = 0; business_runs = false;
    assert(watchdog_start() == 0);
}
static void ready(void) { beat_all(); watchdog_startup_complete(); watchdog_poll(); }
static void advance(unsigned ms, unsigned missing)
{
    assert(ms % 500 == 0);
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 500) {
        now_ms += 500;
        for (unsigned i = 0; i < WATCHDOG_OTA; i++)
            if (!(missing & (1u << i))) watchdog_beat(i);
        watchdog_poll();
    }
}
static const char *field(json_object *o, const char *key)
{ return json_object_get_string(json_object_object_get(o,key)); }
int main(void)
{
    boot(0,32); ready(); advance(600000,0); assert(!failed && feeds > 1000);
    /* Each mandatory worker independently gates feeding. Late recovery, OTA
     * progress, lease requests and startup-complete cannot clear a fault. */
    for (unsigned slot = 0; slot < WATCHDOG_OTA; slot++) {
        boot(0,1); ready();
        advance(slot == WATCHDOG_NETWORK ? 30000 : 10000, 1u << slot);
        assert(failed && !strcmp(fault,names[slot])); unsigned stopped = feeds;
        beat_all(); watchdog_network_end(); watchdog_ota_end(); watchdog_startup_complete();
        assert(!watchdog_network_begin() && !watchdog_ota_begin()); watchdog_poll();
        assert(feeds == stopped);
    }
    boot(0,0); ready(); now_ms = WATCHDOG_LOCAL_MS;
    watchdog_beat(WATCHDOG_HARDWARE); assert(failed); /* Even before daemon polls. */
    boot(0,0); now_ms = WATCHDOG_STARTUP_MS; watchdog_startup_complete();
    assert(failed); /* No first heartbeat is not an empty/healthy registry. */
    boot(0,0); beat_all(); advance(60000,0); assert(failed && !strcmp(fault,"startup"));
    boot(0,0); watchdog_startup_complete();
    advance(60000,1u << WATCHDOG_BINDING); assert(failed);
    boot(UINT32_MAX-5000,1); ready(); advance(30000,0); assert(!failed);
    advance(10000,1u << WATCHDOG_HARDWARE); assert(failed);

    boot(0,8); ready(); assert(watchdog_network_begin());
    assert(!watchdog_network_begin());
    advance(179500,1u << WATCHDOG_NETWORK); assert(!failed);
    watchdog_network_end(); advance(30000,0); assert(!failed);
    assert(watchdog_network_begin());
    advance(180000,0); assert(failed && !strcmp(fault,"network")); /* No renewal by beat. */
    boot(0,8); ready(); assert(watchdog_network_begin());
    advance(10000,(1u<<WATCHDOG_NETWORK)|(1u<<WATCHDOG_BINDING));
    assert(failed && !strcmp(fault,"binding"));

    boot(0,1); ready(); assert(watchdog_ota_begin()); assert(!watchdog_ota_begin());
    advance(29500,0); assert(watchdog_ota_progress()); watchdog_ota_end();
    advance(60000,0); assert(!failed); /* Waiting for upload/network is not watched. */
    assert(watchdog_ota_begin()); advance(30000,0);
    assert(failed && !strcmp(fault,"ota"));
    unsigned stopped = feeds; assert(!watchdog_ota_progress()); watchdog_ota_end(); watchdog_poll();
    assert(feeds == stopped);
    boot(0,1); ready(); assert(watchdog_ota_begin());
    advance(10000,1u<<WATCHDOG_HARDWARE); assert(failed);

    boot(0,1); ready(); business_runs = true; assert(watchdog_ota_begin());
    assert(!watchdog_erase_ota(4097) && erases == 0);
    assert(watchdog_erase_ota(128*4096) && erases == 128 && !failed);
    uint8_t block[512];
    assert(watchdog_read_ota(0,block,sizeof(block)) && reads == 1 && block[0] == 0x55);
    watchdog_ota_end(); assert(!points[WATCHDOG_OTA].active);
    erases = 0; erase_error = 2; assert(watchdog_ota_begin());
    assert(!watchdog_erase_ota(8192) && erases == 2); watchdog_ota_end(); assert(!failed);

    const int reasons[] = {32,8,16,1,2,4,0,-1,40};
    const char *labels[] = {"watchdog","software","lockup","power_on","power_on","power_on","unknown","unknown","watchdog"};
    for (unsigned i = 0; i < sizeof(reasons)/sizeof(reasons[0]); i++) {
        boot(0,reasons[i]); ready(); reset_bits = 0; /* SDK value is cached. */
        watchdog_boot_event(); json_object *o = json_object_new_object();
        watchdog_status_json(o);
        assert(!strcmp(field(o,"reset_reason"),labels[i]));
        assert(json_object_get_int(json_object_object_get(o,"reset_reason_raw")) == reasons[i]);
        assert(!strcmp(field(o,"watchdog_state"),"healthy")); json_object_put(o);
    }
    /* A stopped monitor or stalled scheduler has no independent feeder. */
    boot(0,1); ready(); stopped = feeds; now_ms += WATCHDOG_HARDWARE_MS;
    assert(feeds == stopped && (uint32_t)(now_ms-last_feed) >= WATCHDOG_HARDWARE_MS);
    boot(0,1); enabled = false; started = false; init_error = -1;
    assert(watchdog_start() == -1 && failed && !enabled); watchdog_poll(); assert(feeds == 0);
    boot(0,1); enabled = false; started = false; thread_error = -2;
    assert(watchdog_start() == -2 && failed && enabled); watchdog_poll(); assert(feeds == 0);
    /* Recreating an intentionally paused binding worker after the boot
     * grace period must preserve its first-heartbeat history. */
    boot(0,0);ready();advance(90000,0);
    watchdog_binding_enable(false);advance(20000,1u<<WATCHDOG_BINDING);
    watchdog_binding_enable(true);watchdog_beat(WATCHDOG_BINDING);watchdog_poll();assert(!failed);
    puts("Watchdog: worker deadlines, latch, startup, wraparound, NTP/OTA bounds, Flash progress, reset reasons and daemon failures passed");
}
