/* Unpruned diagnostics byte oracle; append-only migration fields added below. */
#include "common/device_status.h"
/* On-demand diagnostics. Stable IDs are generated from diagnostics-schema.json.
 * Pairs keep numeric IDs without relaxing the string-map request parser. */
#include "diagnostics_ids.h"
static void reference_diagnostic_field(ph_writer *w,unsigned id){phw_begin(w,NULL,true);phw_u32(w,NULL,id);}
void reference_device_diagnostics_mp(ph_writer *w,const device_status_t *s,const device_status_full_t *f)
{
 if(!w)return;
 if(!s||!f||!(s->captured&STATUS_DIAGNOSTIC)){w->failed=true;return;}
 char raw[7],frame[41];static const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<3;i++){unsigned b=f->model.a1.panel_raw[i];raw[i*2]=hex[b>>4];raw[i*2+1]=hex[b&15];}raw[6]=0;
 for(unsigned i=0;i<20;i++){unsigned b=f->model.m1.last_frame[i];frame[i*2]=hex[b>>4];frame[i*2+1]=hex[b&15];}frame[40]=0;
 phw_begin(w,NULL,false);phw_u32(w,"schema",PH_DIAGNOSTICS_SCHEMA);
 phw_begin(w,"fields",true);
 if(f->runtime.reset_valid){reference_diagnostic_field(w,PH_DIAG_LAST_RESET_REASON);phw_i32(w,NULL,f->runtime.reset_reason);phw_end(w);}
 {reference_diagnostic_field(w,PH_DIAG_UPTIME_MS);phw_u32(w,NULL,s->now_ms);phw_end(w);}
 {reference_diagnostic_field(w,PH_DIAG_UPTIME_SECONDS);phw_i32(w,NULL,s->now_ms/1000);phw_end(w);}
 if(f->runtime.memory_valid){reference_diagnostic_field(w,PH_DIAG_FREE_MEMORY);phw_i32(w,NULL,f->runtime.free_memory);phw_end(w);}
 if(f->runtime.memory_valid){reference_diagnostic_field(w,PH_DIAG_ALLOCATED_MEMORY);phw_i32(w,NULL,f->runtime.allocated_memory);phw_end(w);}
 if(f->runtime.memory_valid){reference_diagnostic_field(w,PH_DIAG_FREE_MEMORY_CHUNKS);phw_i32(w,NULL,f->runtime.free_memory_chunks);phw_end(w);}
 if(f->runtime.memory_valid){reference_diagnostic_field(w,PH_DIAG_MIN_OBSERVED_FREE_MEMORY);phw_i32(w,NULL,f->runtime.minimum_free);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_RESET_REASON);phw_text(w,NULL,f->watchdog.reset_name);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_RESET_REASON_RAW);phw_i32(w,NULL,f->watchdog.reset_raw);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_WATCHDOG_ENABLED);phw_bool(w,NULL,f->watchdog.enabled);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_WATCHDOG_STATE);phw_text(w,NULL,f->watchdog.failed?"fault":f->watchdog.ready?"healthy":"starting");phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_WATCHDOG_FAULT);phw_text(w,NULL,f->watchdog.fault);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_OK);phw_i32(w,NULL,(int)f->http.accepted);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_FAIL);phw_i32(w,NULL,(int)f->http.accept_failures);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_ERRNO);phw_i32(w,NULL,f->http.accept_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_LAST_FD);phw_i32(w,NULL,f->http.last_fd);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_OK);phw_i32(w,NULL,(int)f->http.queued);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_REJECT);phw_i32(w,NULL,(int)f->http.rejected);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_FCNTL_FAIL);phw_i32(w,NULL,(int)f->http.nonblock_failures);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_DRAINED);phw_i32(w,NULL,(int)f->http.drained);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_OK);phw_i32(w,NULL,(int)f->http.busy_sent);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_FAIL);phw_i32(w,NULL,(int)f->http.busy_failed);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_ERRNO);phw_i32(w,NULL,f->http.busy_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_BUSY_SEND_BYTES);phw_i32(w,NULL,(int)f->http.busy_bytes);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_OVERFLOW_CLOSE);phw_i32(w,NULL,(int)f->http.rejected);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_ACCEPT_QUEUE_CAPACITY);phw_i32(w,NULL,PH_HTTP_WAIT_SLOTS);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_REQUESTED);phw_i32(w,NULL,PH_HTTP_WAIT_SLOTS+1);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_SET_RC);phw_i32(w,NULL,f->http.set_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_SET_ERRNO);phw_i32(w,NULL,f->http.set_errno);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_GET_RC);phw_i32(w,NULL,f->http.limit_get_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1&&f->http.limit_get_rc==0){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_LIMIT_READBACK);phw_i32(w,NULL,f->http.limit);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_CONNECTIONS_GET_RC);phw_i32(w,NULL,f->http.connections_get_rc);phw_end(w);}
 if(s->model!=DEVICE_TC1&&f->http.connections_get_rc==0){reference_diagnostic_field(w,PH_DIAG_HTTP_TCP_CONNECTIONS);phw_i32(w,NULL,f->http.connections);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_MQTT_ERRORS);phw_i32(w,NULL,f->network.mqtt_errors);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_MQTT_LAST_ERROR);phw_text(w,NULL,f->network.mqtt_last_error);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_SIGNATURE_VERIFIED);phw_bool(w,NULL,f->ota.signature_verified);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_IMAGE_VERIFIED);phw_bool(w,NULL,f->ota.image_verified);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_KEY_ID);phw_i32(w,NULL,f->ota.key_id);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_IMAGE_LENGTH);phw_i32(w,NULL,f->ota.image_length);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_RELEASE_COUNTER);phw_text(w,NULL,f->ota.release_counter);phw_end(w);}
 if(s->model!=DEVICE_TC1){reference_diagnostic_field(w,PH_DIAG_OTA_VERSION);phw_text(w,NULL,f->ota.version);phw_end(w);}
 if(s->model==DEVICE_A1){reference_diagnostic_field(w,PH_DIAG_PANEL_ERRORS);phw_i32(w,NULL,f->model.a1.panel_errors);phw_end(w);}
 if(s->model==DEVICE_A1){reference_diagnostic_field(w,PH_DIAG_HARDWARE_ERRORS);phw_i32(w,NULL,f->model.a1.hardware_errors);phw_end(w);}
 if(s->model==DEVICE_A1){reference_diagnostic_field(w,PH_DIAG_PANEL_LAST_ERROR);phw_i32(w,NULL,f->model.a1.panel_last_error);phw_end(w);}
 if(s->model==DEVICE_A1){reference_diagnostic_field(w,PH_DIAG_PANEL_RAW);phw_text(w,NULL,raw);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_RX_FRAMES);phw_i32(w,NULL,f->model.m1.rx_frames);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_UNKNOWN_FRAMES);phw_i32(w,NULL,f->model.m1.unknown_frames);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_UART_ERRORS);phw_i32(w,NULL,f->model.m1.uart_errors);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_DISCARDED_BYTES);phw_i32(w,NULL,f->model.m1.discarded_bytes);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_LAST_FRAME);phw_text(w,NULL,frame);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_PM25_RAW);phw_i32(w,NULL,s->sensor.m1.sample.pm25_status);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_CLIMATE_RAW);phw_i32(w,NULL,s->sensor.m1.sample.climate_status);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SENSOR_HEALTH_AUXILIARY_RAW);phw_i32(w,NULL,s->sensor.m1.sample.auxiliary_status);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_SERIAL);phw_text(w,NULL,s->sensor.m1.co2.serial);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_ERRORS);phw_i32(w,NULL,s->sensor.m1.co2.errors);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_LAST_ERROR);phw_i32(w,NULL,s->sensor.m1.co2.last_error);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_SAMPLES);phw_i32(w,NULL,f->model.m1.co2_samples);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_CRC_ERRORS);phw_i32(w,NULL,f->model.m1.co2_crc_errors);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_BUS_INITIALIZED);phw_bool(w,NULL,f->model.m1.co2_bus_initialized);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_LAST_COMMAND);phw_i32(w,NULL,f->model.m1.co2_last_command);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_SCD40_ERROR_PHASE);phw_i32(w,NULL,f->model.m1.co2_error_phase);phw_end(w);}
 if(s->model==DEVICE_M1&&m1_barometer_model(s->sensor.m1.barometer.chip_id)){reference_diagnostic_field(w,PH_DIAG_BAROMETER_CHIP_ID);phw_i32(w,NULL,s->sensor.m1.barometer.chip_id);phw_end(w);}
 if(s->model==DEVICE_M1&&m1_barometer_model(s->sensor.m1.barometer.chip_id)){reference_diagnostic_field(w,PH_DIAG_BAROMETER_ADDRESS);phw_i32(w,NULL,s->sensor.m1.barometer.address);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BAROMETER_SAMPLES);phw_i32(w,NULL,s->sensor.m1.barometer.samples);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BAROMETER_ERRORS);phw_i32(w,NULL,s->sensor.m1.barometer.errors);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BAROMETER_ATTEMPTS);phw_i32(w,NULL,s->sensor.m1.barometer.attempts);phw_end(w);}
 if(s->model==DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BAROMETER_LAST_ERROR);phw_i32(w,NULL,s->sensor.m1.barometer.last_error);phw_end(w);}
#ifdef PHICOMM_BEMFA
 if(s->model!=DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BEMFA_ERRORS);phw_i32(w,NULL,s->network.bemfa_errors);phw_end(w);}
#endif
#ifdef PHICOMM_BEMFA
 if(s->model!=DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BEMFA_LAST_STAGE);phw_i32(w,NULL,s->network.bemfa_last_stage);phw_end(w);}
#endif
#ifdef PHICOMM_BEMFA
 if(s->model!=DEVICE_M1){reference_diagnostic_field(w,PH_DIAG_BEMFA_LAST_ERROR);phw_i32(w,NULL,s->network.bemfa_last_error);phw_end(w);}
#endif
 if(s->model==DEVICE_TC1){
  reference_diagnostic_field(w,PH_DIAG_CONFIG_RECOVERY);phw_bool(w,NULL,f->model.tc1.config_recovery);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_CONFIG_SOURCE_VERSION);phw_u32(w,NULL,f->model.tc1.config_source_version);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_CONFIG_AUTH_MAGIC);phw_u32(w,NULL,f->model.tc1.config_auth_magic);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_CONFIG_AUTH_VERSION);phw_u32(w,NULL,f->model.tc1.config_auth_version);phw_end(w);
 }
 if(f->service_memory_valid){
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_RESERVED);phw_u32(w,NULL,f->service_memory.reserved);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_ACTIVE);phw_u32(w,NULL,f->service_memory.active);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_DENIED);phw_u32(w,NULL,f->service_memory.denied);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WAITING);phw_u32(w,NULL,f->service_memory.waiting);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MINIMUM_FREE);phw_u32(w,NULL,f->service_memory.minimum_free);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_PENDING);phw_u32(w,NULL,f->service_memory.mqtt_pending);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_PEAK);phw_u32(w,NULL,f->service_memory.mqtt_peak);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_OVERFLOW);phw_u32(w,NULL,f->service_memory.mqtt_overflow);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_MQTT_MAX_DELAY);phw_u32(w,NULL,f->service_memory.mqtt_max_delay);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WIRE_ALLOC_FAILED);phw_u32(w,NULL,f->service_memory.wire_alloc_failed);phw_end(w);
  reference_diagnostic_field(w,PH_DIAG_SERVICE_MEMORY_WIRE_GUARD_DENIED);phw_u32(w,NULL,f->service_memory.wire_guard_denied);phw_end(w);
 }
 phw_end(w);phw_end(w);
}
