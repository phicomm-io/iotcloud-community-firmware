"""Native MP stack guard rejects missing frames and over-budget paths."""
import contextlib
import io
import unittest
from tools.check_mqtt_core import check,context_frames,mqtt_paths
from tools.check_http_core import StackFrames
SYMBOLS = """a1_automation_execute a1_control_apply a1_status_capture bemfa_command bemfa_config_get bemfa_dispatch bemfa_poll bm_connect bm_poll bm_publish bm_subscribe bm_yield close_session command current device_status_mp device_status_visit discovery discovery_build emit_begin emit_bool emit_double emit_int emit_null emit_string encode_a1 feature_config_save feature_fan_apply filter_discovery guarded_read guarded_write ha_command_input hac_string hac_u16 hac_value hac_ws ingress install_config mp_cached mp_dispatch mp_envelope mqtt_buffer_allocation_permit mqtt_socket_read mqtt_worker mutex_lock open_session owner ph_buf_free ph_buf_reserve ph_mqtt_client_init ph_mqtt_connect ph_mqtt_disconnect ph_mqtt_enqueue_generation ph_mqtt_enqueue_route ph_mqtt_error ph_mqtt_is_owner ph_mqtt_owner ph_mqtt_poll ph_mqtt_protocol_buffers ph_mqtt_publish ph_mqtt_receipt_send_bytes ph_mqtt_record_failure ph_mqtt_record_ingress_drop ph_mqtt_resource_ready ph_mqtt_session_run ph_mqtt_subscribe ph_mqtt_yield ph_wire_free ph_wire_malloc phv_get phv_parse phw_begin phw_bool phw_cached_value phw_i32 phw_int phw_null phw_number phw_take phw_text phw_text_n phw_u32 phw_uint publish publish_owned publish_text publish_wait receipt_retry runtime_fields service_command_mp service_mqtt_command_mp service_mqtt_input service_mqtt_snapshot service_product_capabilities_mp service_product_info_mp service_product_mp service_product_status_mp service_query_mp session_cycle subscribe_plan""".split()
SYMBOLS+=['mqtt_buffer_allocation_failed']
SYMBOLS+=['service_readonly_mp','ph_mqtt_query_consumed']
SYMBOLS+=['service_product_diagnostics_mp','device_diagnostics_mp','a1_diagnostics_capture','service_memory_snapshot','watchdog_status_capture','ph_http_snapshot']
SYMBOLS+=['phw_init_measure','phw_finish_fixed','ph_mqtt_discard_route']
SYMBOLS+=['mp_dispatch_authenticated','ph_buf_new']
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
        for name in ('service_command_mp','service_product_mp','phv_parse','phw_text','ph_wire_malloc','device_status_mp','service_readonly_mp','ph_mqtt_query_consumed'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)

    def test_diagnostic_stack_is_not_omitted(self):
        for name in ['device_diagnostics_mp','service_product_diagnostics_mp','a1_diagnostics_capture']:
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>12288'):
            self.run_budget({'device_diagnostics_mp':13000})

    def test_a1_twelve_kib_budget_includes_sdk_reserve(self):
        self.run_budget({'device_diagnostics_mp':8192})
        with self.assertRaisesRegex(ValueError,'>12288'):
            self.run_budget({'device_diagnostics_mp':8193})

    def test_ha_recursive_rejection_keeps_eight_frames(self):
        out=self.run_budget({'hac_value':100})
        self.assertIn('HA input grammar: 800 application',out)
        with self.assertRaisesRegex(ValueError,'>12288'):
            self.run_budget({'hac_value':1600})

    def test_error_socket_and_drop_callbacks_are_required(self):
        for name in ('ph_mqtt_error','ph_mqtt_record_failure','guarded_read','guarded_write','mqtt_socket_read','ph_mqtt_record_ingress_drop','mqtt_buffer_allocation_failed'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>12288'):
            self.run_budget({'guarded_write':13000})

    def test_native_config_sizing_and_schema_tails_are_budgeted(self):
        frames={'feature_mp':0,'feature_domain_get_mp':100,
                'feature_domain_config_mp':300,'phw_init_measure':200,'phw_init_fixed':0}
        out=self.run_budget(frames)
        self.assertIn('MQTT feature config measure init: 300 application',out)
        self.assertIn('MQTT feature config fields string: 400 application',out)
        with self.assertRaisesRegex(ValueError,'>12288'):
            self.run_budget({**frames,'feature_domain_config_mp':8193})

    def test_error_and_socket_tails_are_exclusive(self):
        out=self.run_budget({'guarded_read':1500,'guarded_write':1500,'ph_mqtt_error':1500,'ph_mqtt_record_failure':1500})
        self.assertIn('MQTT yield error record: 3000 application',out)
        self.assertIn('MQTT yield guarded write: 1500 application',out)

    def test_subscribe_rollback_requires_its_actual_frame_and_owner_tail(self):
        with self.assertRaisesRegex(ValueError,'missing actual .su'):
            self.run_budget(missing='ph_mqtt_discard_route')
        out=self.run_budget({'ph_mqtt_discard_route':100,'ph_mqtt_is_owner':200})
        self.assertIn('MQTT subscribe rollback: 300 application',out)
        self.assertIn('Bemfa subscribe rollback: 300 application',out)
        with self.assertRaisesRegex(ValueError,'>12288|>16384'):
            self.run_budget({'ph_mqtt_discard_route':13000})


class Tc1MqttStackBudget(unittest.TestCase):
    def fixture(self):
        paths,budget=mqtt_paths({},'TC1');frames=StackFrames()
        frames.update({n:16 for names,r in paths.values() for n in names})
        frames.update({n:16 for n in ('mp_dispatch','mp_query','keys','number','boolparam',
            'strparam','tc1_mp_get','tc1_mp_has','phv_scan','mp_receipt_save',
            'mp_receipt_difference','mp_receipt_copy','mp_receipt_release','mp_receipt_apply')})
        return frames,mqtt_paths(frames,'TC1',frames)[0],budget

    def test_control_generic_metadata_and_periodic_queries_use_actual_call_graph(self):
        frames,paths,budget=self.fixture()
        self.assertEqual(budget,8192)
        with contextlib.redirect_stdout(io.StringIO()):check(frames,'TC1',frames)
        for protocol in ('MQTT','HA'):
            generic=paths[protocol+' business generic string'][0]
            control=paths[protocol+' business control number scan'][0]
            self.assertIn('tc1_product_inner_mp',generic);self.assertNotIn('tc1_control_mp',generic)
            self.assertIn('tc1_control_mp',control);self.assertNotIn('tc1_product_inner_mp',control)
            self.assertEqual(control.count('phv_get'),1);self.assertNotIn('boolparam',control)
        metadata=paths['MQTT service_product_info_mp string'][0]
        self.assertFalse(set(metadata)&{'tc1_product_inner_mp','tc1_control_mp'})
        periodic=paths['MQTT periodic status capture'][0]
        self.assertTrue({'service_query_mp','mp_query','mp_dispatch_authenticated'}<=set(periodic))
        self.assertNotIn('mp_dispatch',periodic)
        receipt=paths['MQTT receipt save difference'][0]
        self.assertFalse(set(receipt)&{'mp_receipt_copy','mp_receipt_release','ph_wire_malloc'})

    def test_missing_oversized_and_dynamic_tc1_business_frames_fail(self):
        for name in ('tc1_product_inner_mp','tc1_control_mp','mp_dispatch_authenticated'):
            frames,paths,budget=self.fixture();del frames[name]
            with self.subTest(name=name),contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'missing actual .su'):
                check(frames,'TC1')
            frames,paths,budget=self.fixture();frames[name]=5200
            with self.subTest(name=name),contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>8192'):
                check(frames,'TC1')
        frames,paths,budget=self.fixture();frames.unbounded['tc1_control_mp']='dynamic'
        with contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'unbounded/nonstatic'):
            check(frames,'TC1')

    def test_emitted_wrapper_and_receipt_helpers_require_actual_frames(self):
        for name in ('mp_dispatch','mp_query','mp_receipt_save','mp_receipt_difference',
                     'mp_receipt_copy','mp_receipt_release','mp_receipt_apply'):
            frames,paths,budget=self.fixture();symbols=dict(frames);del frames[name]
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'Emitted helper missing'):
                mqtt_paths(frames,'TC1',symbols)
        frames,paths,budget=self.fixture();frames['mp_receipt_save']=5200
        with contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>8192'):
            check(frames,'TC1',frames)
