/* Exercise the production status producer, including missing heap telemetry.
 * The MOC variant links a fake SDK boot-cause entry point; the portable variant
 * deliberately has no such symbol and must not acquire that dependency. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../common/json_build.h"

typedef struct {
    int num_of_chunks, total_memory, allocted_memory, free_memory;
} micoMemInfo_t;

static uint32_t now;
static micoMemInfo_t memory = {3, 60000, 30000, 30000};
static bool memory_available;
static uint32_t mico_rtos_get_time(void) { return now; }
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

static json_object *sample(uint32_t tick)
{
    now = tick;
    json_object *status = json_object_new_object();
    assert(status);
    runtime_metrics_json(status);
    assert(json_object_to_json_string(status));
    assert(json_object_get_double(json_object_object_get(status, "uptime_ms")) == tick);
#if defined(MOC) && MOC == 1 && defined(CONFIG_CPU_MX1290)
    /* Keep combined flags unchanged, and retain them even if the underlying
     * SDK read is consuming or later returns zero. */
    assert(json_object_get_int(json_object_object_get(status, "last_reset_reason")) == 0x30);
    assert(reset_reason_calls == 1);
#else
    assert(!json_object_object_get(status, "last_reset_reason"));
#endif
    return status;
}

int main(void)
{
    json_object *status = sample(5250);
    assert(!json_object_object_get(status, "free_memory"));
    json_object_put(status);

    memory_available = true;
    status = sample(10000);
    assert(json_object_get_int(json_object_object_get(status, "min_observed_free_memory")) == 30000);
    json_object_put(status);

    memory.free_memory = 19724;
    status = sample(UINT32_MAX);
    assert(json_object_get_int(json_object_object_get(status, "min_observed_free_memory")) == 19724);
    json_object_put(status);

    memory.free_memory = 28596;
    status = sample(10); /* Tick rollover does not reset the sampled heap minimum. */
    assert(json_object_get_int(json_object_object_get(status, "min_observed_free_memory")) == 19724);
    assert(json_object_get_int(json_object_object_get(status, "free_memory")) == 28596);
    json_object_put(status);
    puts("runtime metrics boot cause and sampled heap checks passed");
    return 0;
}
