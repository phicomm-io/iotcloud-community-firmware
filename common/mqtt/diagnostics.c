/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostics.h"
void ph_mqtt_diag_write_mp(ph_writer *w,bool available)
{
    if(!available){phw_bool(w,"mqtt_diag_unavailable",true);return;}
    ph_mqtt_diagnostics d;ph_mqtt_snapshot(&d);
    phw_uint(w,"mqtt_rx_queued",d.queued[0]);
    phw_uint(w,"mqtt_tx_queued",d.queued[1]);
    phw_uint(w,"mqtt_inflight",d.inflight[0]+d.inflight[1]);
    phw_uint(w,"mqtt_queue_bytes",d.bytes);
    phw_uint(w,"mqtt_peak_bytes",d.peak_bytes);
    phw_uint(w,"mqtt_credit_bytes",d.credit);
    phw_uint(w,"mqtt_rejected",d.rejected[0]>UINT32_MAX-d.rejected[1]?UINT32_MAX:d.rejected[0]+d.rejected[1]);
    phw_uint(w,"mqtt_owner_faults",d.owner_faults);
    phw_uint(w,"mqtt_generation",d.generation);
    phw_uint(w,"mqtt_rx_buffer_bytes",d.rx_buffer_bytes);
    phw_uint(w,"mqtt_tx_buffer_bytes",d.tx_buffer_bytes);
    phw_uint(w,"mqtt_protocol_depth",d.protocol_depth);
    phw_uint(w,"mqtt_operation",d.operation);
    phw_uint(w,"mqtt_last_error_stage",d.last_stage);
    phw_uint(w,"mqtt_errors",d.errors);
    phw_uint(w,"mqtt_reconnects",d.reconnects);
    phw_uint(w,"mqtt_max_queue_delay_ms",d.max_delay_ms);
    phw_uint(w,"mqtt_ingress_dropped",d.ingress_dropped);
    phw_uint(w,"mqtt_busy_overflow",d.busy_overflow);
    phw_int(w,"mqtt_last_error",d.last_error);
    phw_begin(w,"mqtt_failure",false);
    phw_u32(w,"reason",d.failure.reason);
    phw_u32(w,"operation",d.failure.operation);
    phw_u32(w,"free",d.failure.free_bytes);
    phw_u32(w,"packet",d.failure.packet_bytes);
    phw_u32(w,"credit",d.failure.credit_bytes);
    phw_u32(w,"queue",d.failure.queue_bytes);
    phw_u32(w,"rx_state",d.failure.rx_state);
    phw_begin(w,"read",true);
    phw_int(w,NULL,d.failure.read.requested);
    phw_int(w,NULL,d.failure.read.returned);
    phw_int(w,NULL,d.failure.read.timeout_ms);
    phw_int(w,NULL,d.failure.read.elapsed_ms);
    phw_end(w);
    phw_begin(w,"write",true);
    phw_int(w,NULL,d.failure.write.requested);
    phw_int(w,NULL,d.failure.write.returned);
    phw_int(w,NULL,d.failure.write.timeout_ms);
    phw_int(w,NULL,d.failure.write.elapsed_ms);
    phw_end(w);
    phw_end(w);
}
