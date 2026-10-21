/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostics.h"
#include <stdio.h>
#include <string.h>
size_t ph_mqtt_diag_append(char *out,size_t capacity,size_t used,bool available) {
    if(used<2 || used>=capacity || memcmp(out+used-2,"}}",2))return 0;
    int n;
    if(!available)n=snprintf(out+used-2,capacity-used+2,",\"mqtt_diag_unavailable\":true}}");
    else {
        ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
        n=snprintf(out+used-2,capacity-used+2,
            ",\"mqtt_rx_queued\":%u,\"mqtt_tx_queued\":%u,\"mqtt_inflight\":%u,"
            "\"mqtt_queue_bytes\":%lu,\"mqtt_peak_bytes\":%lu,\"mqtt_credit_bytes\":%lu,"
            "\"mqtt_rejected\":%lu,\"mqtt_owner_faults\":%lu,\"mqtt_generation\":%u,"
            "\"mqtt_rx_buffer_bytes\":%lu,\"mqtt_tx_buffer_bytes\":%lu,\"mqtt_protocol_depth\":%u,\"mqtt_operation\":%u,"
            "\"mqtt_last_error_stage\":%u,\"mqtt_last_error\":%d,\"mqtt_errors\":%lu,\"mqtt_reconnects\":%lu,"
            "\"mqtt_max_queue_delay_ms\":%lu}}",
            d.queued[0],d.queued[1],d.inflight[0]+d.inflight[1],
            (unsigned long)d.bytes,(unsigned long)d.peak_bytes,(unsigned long)d.credit,
            (unsigned long)(d.rejected[0]>UINT32_MAX-d.rejected[1]?UINT32_MAX:d.rejected[0]+d.rejected[1]),
            (unsigned long)d.owner_faults,d.generation,(unsigned long)d.rx_buffer_bytes,
            (unsigned long)d.tx_buffer_bytes,d.protocol_depth,d.operation,d.last_stage,d.last_error,
            (unsigned long)d.errors,(unsigned long)d.reconnects,(unsigned long)d.max_delay_ms);
    }
    return n>0 && (size_t)n<capacity-used+2?used-2+(size_t)n:0;
}
