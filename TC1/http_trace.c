#include "mico.h"
#include "http_trace.h"
#include "../common/generated/build_version.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define TRACE_MAGIC 0x48545231u
#define TRACE_SLOTS 8u
typedef struct {
    uint32_t sequence, phase, milliseconds;
    int32_t fd, detail;
    uint32_t free_memory, seal;
} trace_record_t;
typedef struct {
    uint32_t magic, build, sequence, minimum_free;
    trace_record_t records[TRACE_SLOTS];
} trace_store_t;
/* Linker keeps this before cleared BSS, outside the heap starting at bss_end.
 * Warm SRAM retention depends on the hardware/kernel and is validated on DUT.
 * This is not Flash, AON scratch, or a persistent settings change. */
static volatile trace_store_t retained __attribute__((section(".http_retained"),used,aligned(4)));
static trace_store_t previous;
static bool initialized;
static uint32_t sampled_free;

static void trace_initialize(void)
{
    if (initialized) return;
    uint32_t build=2166136261u;
    for (const char *s=FIRMWARE_VERSION; *s; ++s) build=(build^(unsigned char)*s)*16777619u;
    if (retained.magic==TRACE_MAGIC && retained.build==build) previous=retained;
    memset((void *)&retained,0,sizeof(retained));
    retained.magic=TRACE_MAGIC;retained.build=build;retained.minimum_free=UINT32_MAX;
    initialized=true;
}

/* Only the HTTP owner writes. No locks, allocation, logging, Flash or feeding. */
void phicomm_http_trace(unsigned phase, int fd, int detail)
{
    int saved_errno=errno;
    trace_initialize();
    uint32_t now=mico_rtos_get_time();
    if (phase<=HTTP_TRACE_HANDLE_RETURN || phase>=HTTP_TRACE_HEADERS_ENTER) {
        const micoMemInfo_t *m=MicoGetMemoryInfo();
        sampled_free=m && m->free_memory>0?(uint32_t)m->free_memory:0;
        if (sampled_free<retained.minimum_free) retained.minimum_free=sampled_free;
    }
    uint32_t seq=retained.sequence+1u;
    if (!seq) seq=1;
    volatile trace_record_t *r=&retained.records[(seq-1)%TRACE_SLOTS];
    r->seal=0;r->sequence=seq;r->phase=phase;r->milliseconds=now;
    r->fd=fd;r->detail=detail;r->free_memory=sampled_free;
    __asm volatile ("" ::: "memory");
    r->seal=~seq;retained.sequence=seq;
    errno=saved_errno;
}

size_t tc1_http_trace_format(char *output, size_t capacity)
{
    trace_initialize();
    bool valid=previous.magic==TRACE_MAGIC;
    int n=snprintf(output,capacity,"{\"code\":\"ok\",\"data\":{\"previous_valid\":%s,\"minimum_sampled_free\":%lu,\"records\":[",
        valid?"true":"false",(unsigned long)(valid?previous.minimum_free:0));
    if (n<0 || (size_t)n>=capacity) return 0;
    size_t used=n;unsigned count=0;
    /* Bounded even if warm SRAM was lost/corrupted. Validate each commit. */
    for (unsigned index=0;valid && index<TRACE_SLOTS;index++) {
        uint32_t seq=previous.sequence-(TRACE_SLOTS-1-index);
        if (!seq) continue;
        const trace_record_t *r=&previous.records[(seq-1)%TRACE_SLOTS];
        if (r->sequence!=seq || r->seal!=~seq || r->phase<1 || r->phase>HTTP_TRACE_CLIENT_SELECT_RETURN) continue;
        n=snprintf(output+used,capacity-used,"%s{\"seq\":%lu,\"stage\":%lu,\"ms\":%lu,\"fd\":%ld,\"detail\":%ld,\"heap\":%lu}",
            count?",":"",(unsigned long)seq,(unsigned long)r->phase,(unsigned long)r->milliseconds,
            (long)r->fd,(long)r->detail,(unsigned long)r->free_memory);
        if (n<0 || (size_t)n>=capacity-used) return 0;
        used+=n;count++;
    }
    n=snprintf(output+used,capacity-used,"]}}");
    return n<0 || (size_t)n>=capacity-used?0:used+(size_t)n;
}
