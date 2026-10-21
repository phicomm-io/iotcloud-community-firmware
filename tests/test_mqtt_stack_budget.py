"""Reject unsafe/missing diagnostic callback frames and keep siblings separate."""
import contextlib
import io
import unittest
from tools.check_mqtt_core import check

# Independent emitted-frame fixture: sizes are injected by the scenarios below.
# Inlined helpers (end, states_step, direct_leaf, direct_json_claimed) are omitted.
SYMBOLS = '''mqtt_worker ph_mqtt_session_run ph_mqtt_poll command service_mqtt_input
service_query service_command service_command_execute service_product service_product_inner
json_owned_to_wire json_write json_primitive json_append ph_mqtt_publish ingress
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
service_mqtt_send ph_mqtt_app_error ph_mqtt_record_operation'''.split()

class MqttStackBudget(unittest.TestCase):
    def run_budget(self, sizes=None, missing=None):
        frames=dict.fromkeys(SYMBOLS,0)
        frames.update(sizes or {})
        if missing: del frames[missing]
        output=io.StringIO()
        with contextlib.redirect_stdout(output): check(frames,'A1')
        return output.getvalue()

    def test_application_source_tail_is_required_and_exclusive(self):
        output=self.run_budget({'json_write':6000,'ph_mqtt_app_error':6000})
        self.assertIn('MQTT filter discovery encoding source marker: 6000 application',output)
        for name in ('ph_mqtt_app_error','ph_mqtt_record_operation','service_mqtt_send'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError,'missing actual .su'):
                self.run_budget(missing=name)
        with self.assertRaisesRegex(ValueError,'>16384'):
            self.run_budget({'ph_mqtt_app_error':13000})

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
