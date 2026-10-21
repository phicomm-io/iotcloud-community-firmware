"""Reject unsafe/missing diagnostic callback frames and keep siblings separate."""
import contextlib
import io
import tempfile
from pathlib import Path
from tools.check_http_core import read_json_allocator_frames
import unittest
from tools.check_mqtt_core import check

# Independent emitted-frame fixture: sizes are injected by the scenarios below.
# Inlined helpers (end, states_step, direct_leaf, direct_json_claimed) are omitted.
SYMBOLS = '''mqtt_worker ph_mqtt_session_run ph_mqtt_poll command service_mqtt_input
service_query service_command service_command_execute service_product service_product_inner
json_owned_to_wire json_owned_to_wire_result json_write json_primitive json_append ph_mqtt_publish ingress
ph_mqtt_enqueue_route ph_mqtt_enqueue_generation session_cycle ph_mqtt_startup_wait
ph_mqtt_owner_init open_session ph_mqtt_connect subscribe_plan ph_mqtt_subscribe
ph_mqtt_yield service_mqtt_snapshot service_mqtt_publish service_mqtt_try_capabilities
a1_status_json a1_status_json_view a1_status_full a1_status_capture device_status_encode
encode_a1 a1_control_local feature_group_control group_send parse feature_command
feature_mqtt_input feature_mqtt_service virtual_service discovery_virtual_step
json_owned_publish checked_publish publish discovery filter_discovery ota_job_poll
feature_resume_step ph_mqtt_client_init publish_text close_session ph_mqtt_disconnect
ph_mqtt_error ph_mqtt_record_failure ph_mqtt_is_owner current mutex_lock guarded_read
guarded_write ph_mqtt_protocol_buffers mqtt_socket_read ph_mqtt_record_ingress_drop
service_mqtt_optional_send service_mqtt_json_begin service_mqtt_json_end
service_mqtt_json_error json_heap_failure json_heap_malloc json_heap_calloc
json_heap_realloc json_heap_strdup json_heap.permit json_heap.created discovery_build ph_mqtt_app_error ph_mqtt_record_operation
service_json_scope_begin service_json_scope_failed service_json_scope_end service_json_release
service_caller_graph_failed json_heap_lock ph_mqtt_owner_thread ph_mqtt_owner direct_json_optional'''.split()

class MqttStackBudget(unittest.TestCase):
    def run_budget(self, sizes=None, missing=None):
        frames=dict.fromkeys(SYMBOLS,0)
        frames.update(sizes or {})
        if missing: del frames[missing]
        output=io.StringIO()
        with contextlib.redirect_stdout(output): check(frames,'A1')
        return output.getvalue()

    def test_caller_scope_and_boolean_helper_tails_are_counted(self):
        output=self.run_budget({'service_caller_graph_failed':40,'json_heap_lock':8,
                                'service_json_scope_end':32,'status_bool':24,'direct_json_optional':96})
        self.assertIn('MQTT command failure attribution: 48 application',output)
        self.assertIn('MQTT filter discovery encoding cleanup: 40 application',output)
        self.assertIn('MQTT status pure encode: 24 application',output)
        self.assertIn('MQTT virtual domain encoding cleanup: 136 application',output)
        for name in ('service_json_scope_begin','service_json_scope_failed',
                     'service_json_scope_end','service_json_release',
                     'service_caller_graph_failed','json_heap_lock',
                     'ph_mqtt_owner_thread','ph_mqtt_owner','direct_json_optional'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        for name in ('service_caller_graph_failed','service_json_scope_end','status_bool','direct_json_optional'):
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'>16384'):
                self.run_budget({name:13000})

    def test_application_source_tail_is_required_and_exclusive(self):
        output=self.run_budget({'json_write':6000,'ph_mqtt_app_error':6000})
        self.assertIn('MQTT filter discovery encoding source marker: 6000 application',output)
        for name in ('ph_mqtt_app_error','ph_mqtt_record_operation','service_mqtt_optional_send'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'ph_mqtt_app_error':13000})

    def test_snapshot_publication_retains_wrapper_during_cleanup(self):
        output=self.run_budget({'service_mqtt_publish':96,'service_json_scope_end':32,
                                'service_json_release':24,'json_heap_lock':8})
        self.assertIn('MQTT snapshot publication service_json_scope_end: 136 application',output)
        self.assertIn('MQTT snapshot publication service_json_release: 128 application',output)
        # Neither frame alone exceeds the budget. Their real nesting does.
        for tail in ('service_json_scope_end','service_json_release'):
            with self.subTest(tail=tail),self.assertRaisesRegex(ValueError,'>16384'):
                self.run_budget({'service_mqtt_publish':6000,tail:6300})

    def test_optional_scope_and_allocator_tails_are_required(self):
        output=self.run_budget({'json_heap_failure':32,'service_mqtt_json_finish':24,
                                'service_mqtt_optional_send':40})
        self.assertIn('MQTT filter discovery encoding cleanup: 64 application',output)
        self.assertIn('MQTT filter discovery allocation observation: 72 application',output)
        # Cleanup returned before sender/error marker; it is not retained there.
        self.assertIn('MQTT filter discovery encoding source marker: 40 application',output)
        for name in ('json_owned_to_wire_result','json_heap_failure','service_mqtt_json_begin',
                     'service_mqtt_json_end','service_mqtt_json_error','service_mqtt_optional_send'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'json_heap_failure':13000})
        output=self.run_budget({'json_heap_realloc':16,'json_heap_malloc':8,'json_heap.permit':8,
                                'json_heap.created':8,'json_heap_failure':8})
        self.assertIn('MQTT filter discovery allocation observation json_heap_realloc permit: 40 application',output)
        for name in ('json_heap_malloc','json_heap_calloc','json_heap_realloc','json_heap_strdup','json_heap.permit','json_heap.created'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)


    def test_sdk_allocator_frames_are_isolated_from_application_symbols(self):
        with tempfile.TemporaryDirectory() as directory:
            modules=Path(directory);sdk=modules/'Lib_JSON_C';sdk.mkdir()
            frames={'permit':6000,'created':7000,'json_heap_failure':8,'json_heap_lock':16}
            with self.assertRaisesRegex(ValueError,'Expected one actual SDK'):
                read_json_allocator_frames(modules,frames)
            (sdk/'json_heap.su').write_text('json_heap.c:12:12:permit\t8\tstatic\n'
                'json_heap.c:16:13:created\t8\tstatic\n'
                'json_heap.c:21:7:json_heap_malloc\t8\tstatic\n'
                'json_heap.c:10:28:json_heap_failure\t0\tstatic\n'
                'json_heap.c:7:28:json_heap_lock\t0\tstatic\n')
            read_json_allocator_frames(modules,frames)
            self.assertEqual((frames['permit'],frames['created']),(6000,7000))
            self.assertEqual((frames['json_heap.permit'],frames['json_heap.created']),(8,8))
            self.assertEqual((frames['json_heap_failure'],frames['json_heap_lock']),(8,16))
            (modules/'json_heap.su').write_text('')
            with self.assertRaisesRegex(ValueError,'Expected one actual SDK'):
                read_json_allocator_frames(modules,frames)

    def test_diagnostic_tails_are_reported(self):
        output=self.run_budget({'ph_mqtt_error':40,'ph_mqtt_record_failure':48,
                                'guarded_read':32,'guarded_write':32})
        self.assertIn('MQTT yield error record: 88 application + 4096 reserve',output)
        self.assertIn('MQTT yield guarded write: 32 application + 4096 reserve',output)
        self.assertIn('MQTT yield guarded read adapter: 32 application',output)

    def test_overload_drop_tail_is_exclusive_and_required(self):
        output=self.run_budget({'ph_mqtt_enqueue_generation':6000,
                                'ph_mqtt_record_ingress_drop':6000})
        self.assertIn('MQTT yield ingress drop count: 6000 application',output)
        with self.assertRaisesRegex(ValueError,'missing actual .su'):
            self.run_budget(missing='ph_mqtt_record_ingress_drop')
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'ph_mqtt_record_ingress_drop':13000})

    def test_missing_required_diagnostic_frames_fail(self):
        for name in ('ph_mqtt_error','ph_mqtt_record_failure','guarded_read','guarded_write',
                     'ph_mqtt_is_owner','current','mutex_lock','mqtt_socket_read'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)

    def test_error_and_write_overflow_fail(self):
        for name in ('ph_mqtt_record_failure','guarded_write','mqtt_socket_read'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'>16384'):
                self.run_budget({name:13000})

    def test_exclusive_adapter_and_error_tails_are_not_summed(self):
        # Each fits separately; summing read/write/error siblings would fail.
        output=self.run_budget({'guarded_read':6000,'guarded_write':6000,
                                'ph_mqtt_error':6000,'ph_mqtt_record_failure':6000})
        self.assertIn('MQTT yield error record: 12000 application',output)
        # An emitted end frame really is retained on the error tail.
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'ph_mqtt_error':6000,'ph_mqtt_record_failure':6000,'end':1000})
