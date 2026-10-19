#pragma once
/* The HTTP owner waits on an already-open descriptor. No body/JSON allocation
 * occurs here; the fixed descriptor queue continues to bound pending work. */
static unsigned service_memory_wait(size_t input_bytes) {
    unsigned claim = service_memory_claim(input_bytes);
    if (claim) return claim;
    if (!service_memory_cost(input_bytes)) return 0;
    uint32_t deadline = mico_rtos_get_time() + 2000u;
    service_memory_waiting(true);
    do {
        mico_thread_msleep(25);
        claim = service_memory_claim(input_bytes);
    } while (!claim && (int32_t)(mico_rtos_get_time() - deadline) < 0);
    service_memory_waiting(false);
    return claim;
}
