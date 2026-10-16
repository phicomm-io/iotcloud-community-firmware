#pragma once
/* Included by the product status owner. service_command serializes snapshots;
 * the minimum is the lowest sampled heap value, not an allocator watermark. */
static void runtime_metrics_json(json_object *o)
{
    static int minimum = -1;
#if defined(MOC) && MOC == 1 && defined(CONFIG_CPU_MX1290)
    /* This exact SDK entry point is also used by MK3080B init_platform.
     * Preserve its raw boot flags: a watchdog flag alone cannot distinguish
     * a fault, deadlock or starvation. Do not call into MOC on other targets. */
    extern int get_last_reset_reason(void);
    static bool reset_reason_sampled;
    static int reset_reason;
    if (!reset_reason_sampled) {
        reset_reason = get_last_reset_reason();
        reset_reason_sampled = true;
    }
    json_build_object_add(o, "last_reset_reason", json_object_new_int(reset_reason));
#endif
    uint32_t now = mico_rtos_get_time();
    json_build_object_add(o, "uptime_ms", json_object_new_double((double)now));
    json_build_object_add(o, "uptime_seconds", json_object_new_int(now / 1000));
    const micoMemInfo_t *memory = MicoGetMemoryInfo();
    if (!memory) return;
    micoMemInfo_t snapshot = *memory;
    if (minimum < 0 || snapshot.free_memory < minimum) minimum = snapshot.free_memory;
    json_build_object_add(o, "free_memory", json_object_new_int(snapshot.free_memory));
    json_build_object_add(o, "allocated_memory", json_object_new_int(snapshot.allocted_memory));
    json_build_object_add(o, "free_memory_chunks", json_object_new_int(snapshot.num_of_chunks));
    json_build_object_add(o, "min_observed_free_memory", json_object_new_int(minimum));
}
