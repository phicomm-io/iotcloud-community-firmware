import base64
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools import validate_m1_barometer_captures as validator


VERSION = "0.9.15-02ff3c"


def body(uptime, samples):
    return {"code": "ok", "data": {
        "device_id": "b0f8932864b6", "model": "m1", "version": VERSION,
        "uptime_seconds": uptime,
        "barometer": {"ready": True, "valid": True, "model": "BMP388",
                       "chip_id": 80, "address": 119, "age_ms": 150,
                       "pressure_hpa": 1013.2, "samples": samples},
    }}


class BarometerCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def save(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value))
        return str(path)

    def pair(self, first=None, second=None):
        return [self.save("first.json", first or body(100, 5)),
                self.save("second.json", second or body(110, 6))]

    def test_two_fresh_captures_confirm_sensor_progress(self):
        report = validator.validate_captures(self.pair(), VERSION)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["hardware_presence"], "m1_identity_confirmed")
        self.assertEqual(report["sensor_presence"], "bmp388_hardware_confirmed")

    def test_identity_is_derived_without_an_invented_address(self):
        report = validator.validate_captures(self.pair(), VERSION)
        self.assertEqual(report["expected_device_id"], body(100, 5)["data"]["device_id"])
        self.assertEqual(report["identity_source"], "capture_metadata")
        self.assertIsNone(report["device_ip"])

    def test_explicit_identity_mismatch_and_missing_metadata_fail(self):
        report = validator.validate_captures(
            self.pair(), VERSION, expected_device_id="020000000099")
        self.assertEqual(report["status"], "failed")
        self.assertIn("capture_0_identity_or_version_mismatch", report["failures"])
        first, second = body(100, 5), body(110, 6)
        del first["data"]["device_id"]
        del second["data"]["device_id"]
        report = validator.validate_captures(self.pair(first, second), VERSION)
        self.assertIn("device_identity_missing_or_invalid", report["failures"])

    def test_explicit_address_requires_matching_capture_metadata(self):
        first, second = body(100, 5), body(110, 6)
        first["capture_metadata"] = {"device_ip": "198.51.100.10"}
        second["capture_metadata"] = {"device_ip": "198.51.100.10"}
        report = validator.validate_captures(
            self.pair(first, second), VERSION, expected_ip="198.51.100.10")
        self.assertEqual(report["status"], "passed")
        second["capture_metadata"]["device_ip"] = "198.51.100.20"
        report = validator.validate_captures(
            self.pair(first, second), VERSION, expected_ip="198.51.100.10")
        self.assertIn("capture_1_device_ip_missing_or_mismatch", report["failures"])

    def test_insufficient_captures_are_not_run(self):
        report = validator.validate_captures([], VERSION)
        self.assertEqual(report["status"], "NOT_RUN")
        self.assertEqual(report["hardware_presence"], "not_confirmed")

    def test_identity_mismatch_fails(self):
        wrong = body(110, 6)
        wrong["data"]["device_id"] = "different"
        report = validator.validate_captures(self.pair(second=wrong), VERSION)
        self.assertEqual(report["status"], "failed")
        self.assertIn("capture_1_identity_or_version_mismatch", report["failures"])

    def test_wrong_sensor_identity_fails(self):
        wrong = body(110, 6)
        wrong["data"]["barometer"]["chip_id"] = 96
        report = validator.validate_captures(self.pair(second=wrong), VERSION)
        self.assertEqual(report["status"], "failed")
        self.assertIn("capture_1_not_expected_bmp388_at_0x77", report["failures"])

    def test_missing_sensor_is_distinguished_from_not_run(self):
        missing = body(110, 6)
        del missing["data"]["barometer"]
        report = validator.validate_captures(self.pair(second=missing), VERSION)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["sensor_presence"], "sensor_not_present_in_response")

    def test_stale_nonfinite_and_missing_pressure_fail(self):
        stale = body(110, 6)
        stale["data"]["barometer"]["age_ms"] = 10000
        stale["data"]["barometer"]["pressure_hpa"] = None
        report = validator.validate_captures(self.pair(second=stale), VERSION)
        self.assertIn("capture_1_stale_or_invalid_age", report["failures"])
        self.assertIn("capture_1_pressure_missing_or_nonfinite", report["failures"])

    def test_nonincreasing_samples_and_uptime_regression_fail(self):
        report = validator.validate_captures(self.pair(second=body(99, 5)), VERSION)
        self.assertIn("capture_1_uptime_regressed", report["failures"])
        self.assertIn("capture_1_sensor_sample_counter_did_not_increase",
                      report["failures"])

    def test_source_capture_validation_records_pointer_hashes_and_times(self):
        baseline = self.root / "runner.json"
        baseline.write_text(json.dumps({"before": {"10.117.152.110": {
            "state": body(100, 5)["data"],
            "status_request_started_monotonic": 10.0,
            "status_request_ended_monotonic": 10.2}}}))
        raw = json.dumps(body(101, 6)).encode()
        sample_doc = self.root / "http.json"
        sample_doc.write_text(json.dumps({"samples": [{
            "ip": "10.117.152.110", "view": "live",
            "request_started_monotonic": 12.0, "request_ended_monotonic": 12.2,
            "raw_body_base64": base64.b64encode(raw).decode()}]}))
        report = validator.validate_source_captures([
            {"path": str(baseline), "json_pointer": "/before/10.117.152.110",
             "kind": "runner_state"},
            {"path": str(sample_doc), "json_pointer": "/samples/0",
             "kind": "http_sample"},
        ], VERSION)
        self.assertEqual(report["status"], "passed")
        provenance = report["samples"][1]["provenance"]
        self.assertEqual(provenance["json_pointer"], "/samples/0")
        self.assertEqual(provenance["raw_body_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(provenance["request_started_monotonic"], 12.0)

    def test_source_capture_interval_too_close_fails(self):
        baseline = self.root / "runner.json"
        baseline.write_text(json.dumps({"before": {"10.117.152.110": {
            "state": body(100, 5)["data"],
            "status_request_started_monotonic": 10.0,
            "status_request_ended_monotonic": 10.2}}}))
        raw = json.dumps(body(101, 6)).encode()
        sample_doc = self.root / "http.json"
        sample_doc.write_text(json.dumps({"samples": [{
            "ip": "10.117.152.110", "request_started_monotonic": 10.5,
            "request_ended_monotonic": 10.7,
            "raw_body_base64": base64.b64encode(raw).decode()}]}))
        report = validator.validate_source_captures([
            {"path": str(baseline), "json_pointer": "/before/10.117.152.110",
             "kind": "runner_state"},
            {"path": str(sample_doc), "json_pointer": "/samples/0",
             "kind": "http_sample"},
        ], VERSION)
        self.assertIn("capture_1_too_close_to_previous_capture", report["failures"])

    def test_boolean_counter_and_reversed_timestamp_fail(self):
        baseline = self.root / "runner.json"
        baseline.write_text(json.dumps({"before": {"10.117.152.110": {
            "state": body(100, 5)["data"],
            "status_request_started_monotonic": 10.0,
            "status_request_ended_monotonic": 10.2}}}))
        bad = body(101, True)
        raw = json.dumps(bad).encode()
        sample_doc = self.root / "http.json"
        sample_doc.write_text(json.dumps({"samples": [{
            "request_started_monotonic": 12.3, "request_ended_monotonic": 12.2,
            "raw_body_base64": base64.b64encode(raw).decode()}]}))
        report = validator.validate_source_captures([
            {"path": str(baseline), "json_pointer": "/before/10.117.152.110",
             "kind": "runner_state"},
            {"path": str(sample_doc), "json_pointer": "/samples/0",
             "kind": "http_sample"},
        ], VERSION)
        self.assertIn("capture_1_sample_counter_invalid", report["failures"])
        self.assertIn("capture_1_request_interval_invalid", report["failures"])


if __name__ == "__main__":
    unittest.main()
