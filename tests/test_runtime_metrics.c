/* Exercise the production status producer, including missing heap telemetry.
 * The MOC variant links a fake SDK boot-cause entry point; the portable variant
 * deliberately has no such symbol and must not acquire that dependency. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../common/typed_wire.h"

typedef struct {
    int num_of_chunks, total_memory, allocted_memory, free_memory;
} micoMemInfo_t;

static uint32_t now;
static micoMemInfo_t memory = {3, 60000, 30000, 30000};
static bool memory_available;
static const micoMemInfo_t *MicoGetMemoryInfo(void)
{ return memory_available ? &memory : NULL; }

#if defined(MOC) && MOC == 1 && defined(CONFIG_CPU_MX1290)
static unsigned reset_reason_calls;
int get_last_reset_reason(void)
{
    ++reset_reason_calls;
    return reset_reason_calls == 1 ? 0x30 : 0;
}
#endif

#include "../common/runtime_metrics.h"
#include "../common/device_status.c"

static bool field(ph_buf *wire,const char *key,int64_t *number)
{
    ph_value root,value;assert(phv_parse(PH_WIRE_MSGPACK,wire->buf,wire->len,&root));
    if(!phv_get(&root,key,&value))return false;
    if(number){double n;assert(phv_number(&value,&n));*number=(int64_t)n;}
    return true;
}
static int64_t integer(ph_buf *wire,const char *key){int64_t n;assert(field(wire,key,&n));return n;}
static ph_buf *sample(uint32_t tick)
{
    now = tick;
    device_runtime_t captured;runtime_metrics_capture(&captured,now);
    ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);device_runtime_mp(&w,&captured);phw_end(&w);
    ph_buf *status=phw_take(&w);assert(status);
    assert(integer(status,"uptime_ms")==tick);
#if defined(MOC) && MOC == 1 && defined(CONFIG_CPU_MX1290)
    /* Preserve combined reset flags and cache a consuming SDK read. */
    assert(integer(status,"last_reset_reason")==0x30);
    assert(reset_reason_calls==1);
#else
    assert(!field(status,"last_reset_reason",NULL));
#endif
    return status;
}

int main(void)
{
    ph_buf *status = sample(5250);
    assert(!field(status,"free_memory",NULL));
    ph_buf_free(status);

    memory_available = true;
    status = sample(10000);
    assert(integer(status,"min_observed_free_memory") == 30000);
    ph_buf_free(status);

    memory.free_memory = 19724;
    status = sample(UINT32_MAX);
    assert(integer(status,"min_observed_free_memory") == 19724);
    ph_buf_free(status);

    memory.free_memory = 28596;
    status = sample(10); /* Tick rollover does not reset the sampled heap minimum. */
    assert(integer(status,"min_observed_free_memory") == 19724);
    assert(integer(status,"free_memory") == 28596);
    ph_buf_free(status);
    puts("runtime metrics boot cause and sampled heap checks passed");
    return 0;
}
