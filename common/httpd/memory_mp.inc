/* Included by product HTTP owner after response/product declarations. */
static void memory_response(ph_http_exchange *x)
{
 const micoMemInfo_t *memory=MicoGetMemoryInfo();
 if(!memory){ph_http_message(&product,x,503,"unavailable");return;}
 micoMemInfo_t m=*memory;service_memory_state_t b;service_memory_snapshot(&b);
 ph_writer w;if(!phw_init(&w,PH_WIRE_MSGPACK)){ph_http_busy(x);return;}
 phw_begin(&w,NULL,false);phw_text(&w,"code","ok");phw_begin(&w,"data",false);
 phw_int(&w,"free_memory",m.free_memory);phw_int(&w,"allocated_memory",m.allocted_memory);phw_int(&w,"free_memory_chunks",m.num_of_chunks);phw_u32(&w,"uptime_ms",mico_rtos_get_time());
 phw_u32(&w,"memory_reserve",SERVICE_MEMORY_RESERVE);phw_u32(&w,"memory_reserved",b.reserved);phw_u32(&w,"memory_active",b.active);phw_u32(&w,"memory_waiting",b.waiting);phw_u32(&w,"memory_denied",b.denied);phw_u32(&w,"minimum_sampled_free",b.minimum_free);
 phw_u32(&w,"json_live_blocks",b.json_live);phw_u32(&w,"json_peak_blocks",b.json_peak);phw_u32(&w,"json_alloc_failures",b.json_failed);phw_u32(&w,"json_guard_denied",b.json_denied);
 phw_u32(&w,"mqtt_pending",b.mqtt_pending);phw_u32(&w,"mqtt_peak",b.mqtt_peak);phw_u32(&w,"mqtt_overflow",b.mqtt_overflow);phw_u32(&w,"mqtt_max_delay_ms",b.mqtt_max_delay);
 ph_http_diagnostics d=ph_http_snapshot();
 phw_u32(&w,"http_core_owner_state",d.owner_state);phw_u32(&w,"http_core_restarts",d.restarts);phw_u32(&w,"http_core_close_failures",d.close_failures);phw_int(&w,"http_core_last_close_rc",d.last_close_rc);phw_int(&w,"http_core_last_close_errno",d.last_close_errno);
 phw_u32(&w,"http_core_slot_bytes",d.slot_bytes);phw_u32(&w,"http_core_server_bytes",d.server_bytes);phw_int(&w,"http_core_last_select_rc",d.last_select_rc);phw_int(&w,"http_core_last_read_rc",d.last_read_rc);phw_u32(&w,"http_core_occupied",d.occupied);phw_u32(&w,"http_tcp_sample_ms",d.connections_sample_ms);
 phw_u32(&w,"http_core_accepted",d.accepted);phw_u32(&w,"http_core_rejected",d.rejected);phw_int(&w,"http_core_tcp_limit",d.limit);phw_int(&w,"http_core_tcp_connections",d.connections);phw_int(&w,"http_core_tcp_rc",d.connections_get_rc);phw_int(&w,"http_core_read_errno",d.read_errno);
 phw_begin(&w,"http_core_closed",true);for(unsigned i=0;i<7;i++)phw_u32(&w,NULL,d.closed[i]);phw_end(&w);
#ifdef PH_HTTP_MEMORY_EXTRA
 PH_HTTP_MEMORY_EXTRA(&w);
#endif
 ph_mqtt_diag_write_mp(&w,true);phw_end(&w);phw_end(&w);ph_http_wire_take(&product,x,phw_take(&w));
}
