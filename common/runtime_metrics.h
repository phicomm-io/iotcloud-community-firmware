#pragma once
/* Included by the product status owner. service_command serializes snapshots;
 * the minimum is the lowest sampled heap value, not an allocator watermark. */
#include "device_status.h"
#include <string.h>
static void runtime_metrics_capture(device_runtime_t *out, uint32_t now)
{
    static int minimum = -1;
    memset(out,0,sizeof(*out)); out->now_ms=now;
#if defined(MOC) && MOC == 1 && defined(CONFIG_CPU_MX1290)
    extern int get_last_reset_reason(void);
    static bool sampled; static int reset;
    if(!sampled) { reset=get_last_reset_reason(); sampled=true; }
    out->reset_valid=true;out->reset_reason=reset;
#endif
    const micoMemInfo_t *memory=MicoGetMemoryInfo();
    if(!memory)return;
    micoMemInfo_t m=*memory;
    if(minimum<0 || m.free_memory<minimum)minimum=m.free_memory;
    out->memory_valid=true;out->free_memory=m.free_memory;
    out->allocated_memory=m.allocted_memory;out->free_memory_chunks=m.num_of_chunks;
    out->minimum_free=minimum;
}
