"""Native MP stack guard rejects missing frames and over-budget paths."""
import contextlib
import io
import unittest
from tools.check_mqtt_core import check,context_frames
SYMBOLS = """a1_automation_execute a1_control_apply a1_status_capture bemfa_command bemfa_config_get bemfa_dispatch bemfa_poll bm_connect bm_poll bm_publish bm_subscribe bm_yield close_session command current device_status_mp device_status_visit discovery discovery_build emit_begin emit_bool emit_double emit_int emit_null emit_string encode_a1 feature_config_save feature_fan_apply filter_discovery guarded_read guarded_write ha_command_input hac_string hac_u16 hac_value hac_ws ingress install_config mp_cached mp_dispatch mp_envelope mqtt_buffer_allocation_permit mqtt_socket_read mqtt_worker mutex_lock open_session owner ph_buf_free ph_buf_reserve ph_mqtt_client_init ph_mqtt_connect ph_mqtt_disconnect ph_mqtt_enqueue_generation ph_mqtt_enqueue_route ph_mqtt_error ph_mqtt_is_owner ph_mqtt_owner ph_mqtt_poll ph_mqtt_protocol_buffers ph_mqtt_publish ph_mqtt_receipt_send_bytes ph_mqtt_record_failure ph_mqtt_record_ingress_drop ph_mqtt_resource_ready ph_mqtt_session_run ph_mqtt_subscribe ph_mqtt_yield ph_wire_free ph_wire_malloc phv_get phv_parse phw_begin phw_bool phw_cached_value phw_i32 phw_int phw_null phw_number phw_take phw_text phw_text_n phw_u32 phw_uint publish publish_owned publish_text publish_wait receipt_retry runtime_fields service_command_mp service_mqtt_command_mp service_mqtt_input service_mqtt_snapshot service_product_capabilities_mp service_product_info_mp service_product_mp service_product_status_mp service_query_mp session_cycle subscribe_plan""".split()
SYMBOLS+=['phw_init_measure','phw_finish_fixed']
expanded=[SYMBOLS.copy()];context_frames(expanded);SYMBOLS+=expanded[0]

class MqttStackBudget(unittest.TestCase):
    def run_budget(self,sizes=None,missing=None):
        frames=dict.fromkeys(SYMBOLS,0);frames.update(sizes or {})
        if missing:del frames[missing]
        out=io.StringIO()
        with contextlib.redirect_stdout(out):check(frames,'A1')
        return out.getvalue()

    def test_native_status_and_cache_have_distinct_tails(self):
        out=self.run_budget({'service_product_status_mp':100,'a1_status_capture':200,'phw_cached_value':32})
        self.assertIn('MQTT command status capture: 300 application',out)
        self.assertIn('MQTT command cache: 32 application',out)
        self.assertNotIn('JSON',out)

    def test_missing_public_native_frames_fail_closed(self):
        for name in ('service_command_mp','service_product_mp','phv_parse','phw_text','ph_wire_malloc','device_status_mp'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)

    def test_ha_recursive_rejection_keeps_eight_frames(self):
        out=self.run_budget({'hac_value':100})
        self.assertIn('HA input grammar: 800 application',out)
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'hac_value':1600})

    def test_error_socket_and_drop_callbacks_are_required(self):
        for name in ('ph_mqtt_error','ph_mqtt_record_failure','guarded_read','guarded_write','mqtt_socket_read','ph_mqtt_record_ingress_drop'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'guarded_write':13000})

    def test_error_and_socket_tails_are_exclusive(self):
        out=self.run_budget({'guarded_read':1500,'guarded_write':1500,'ph_mqtt_error':1500,'ph_mqtt_record_failure':1500})
        self.assertIn('MQTT yield error record: 3000 application',out)
        self.assertIn('MQTT yield guarded write: 1500 application',out)
