#!/usr/bin/env python3
"""Offline validator for captured M1 BMP388 liveness evidence.

This validates captures for the measured M1 BMP388 at 0x77 only. It is not a
generic sensor validator or a reboot/accuracy acceptance test. It never
contacts a device; inputs are ordered JSON response envelopes captured at
stage boundaries.
"""
import argparse
import base64
import hashlib
import json
import ipaddress
import re
import math
from pathlib import Path


MODEL = "m1"
BAROMETER_MODEL = "BMP388"
CHIP_ID = 80
ADDRESS = 119
MAX_AGE_MS = 10000


def _load_capture(path):
    path = Path(path)
    raw = path.read_bytes()
    return path, hashlib.sha256(raw).hexdigest(), json.loads(raw.decode("utf-8"))


def _status_data(envelope):
    if not isinstance(envelope, dict) or envelope.get("code") != "ok":
        raise ValueError("response_envelope_invalid")
    data = envelope.get("data")
    if not isinstance(data, dict):
        raise ValueError("response_data_missing")
    return data


def _pointer(document, pointer):
    if not isinstance(pointer, str) or not pointer.startswith("/"):
        raise ValueError("json_pointer_invalid")
    value = document
    for part in pointer[1:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if isinstance(value, list):
            value = value[int(part)]
        elif isinstance(value, dict):
            value = value[part]
        else:
            raise ValueError("json_pointer_does_not_resolve")
    return value


def _device_identity(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{12}", value):
        raise ValueError("device_identity_missing_or_invalid")
    return value.lower()


def _device_ip(value):
    if value is None:
        return None
    try:
        if not isinstance(value, str):
            raise ValueError
        return str(ipaddress.ip_address(value))
    except (TypeError, ValueError):
        raise ValueError("device_ip_invalid") from None


def _capture_ip(envelope, provenance):
    metadata = envelope.get("capture_metadata", {})
    if not isinstance(metadata, dict):
        raise ValueError("capture_metadata_invalid")
    value = (provenance or {}).get("device_ip")
    if value is None:
        value = metadata.get("device_ip", metadata.get("ip", envelope.get("device_ip")))
    return _device_ip(value)


def validate_source_captures(capture_sources, expected_version, expected_device_id=None, expected_ip=None):
    """Validate stage evidence files without issuing any device requests.

    Each source is a mapping with path, json_pointer, and kind. `runner_state`
    points to a Runner before-record; `http_sample` points to a captured HTTP
    schema sample containing its original raw response body.
    """
    captures = []
    source_failures = []
    for index, source in enumerate(capture_sources):
        try:
            path = Path(source["path"])
            source_raw = path.read_bytes()
            source_sha256 = hashlib.sha256(source_raw).hexdigest()
            document = json.loads(source_raw.decode("utf-8"))
            item = _pointer(document, source["json_pointer"])
            body_sha256 = None
            if source["kind"] == "runner_state":
                data = item["state"]
                started = item["status_request_started_monotonic"]
                ended = item["status_request_ended_monotonic"]
            elif source["kind"] == "http_sample":
                raw_body = base64.b64decode(item["raw_body_base64"], validate=True)
                body_sha256 = hashlib.sha256(raw_body).hexdigest()
                envelope = json.loads(raw_body.decode("utf-8"))
                data = _status_data(envelope)
                started = item["request_started_monotonic"]
                ended = item["request_ended_monotonic"]
            else:
                raise ValueError("capture_kind_invalid")
            capture_ip = source.get("device_ip", item.get("ip"))
            if capture_ip is None and source["kind"] == "runner_state":
                candidate = source["json_pointer"].rsplit("/", 1)[-1]
                try:
                    capture_ip = _device_ip(candidate)
                except ValueError:
                    pass
            provenance = {
                "device_ip": _device_ip(capture_ip),
                "source_path": str(path), "source_sha256": source_sha256,
                "json_pointer": source["json_pointer"], "kind": source["kind"],
                "request_started_monotonic": started,
                "request_ended_monotonic": ended,
                "raw_body_sha256": body_sha256,
                "derived_data_sha256": hashlib.sha256(json.dumps(
                    data, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
            }
            captures.append((path, source_sha256, {"code": "ok", "data": data}, provenance))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError,
                KeyError, TypeError, IndexError) as error:
            source_failures.append("capture_%d_source_%s" % (index, str(error)))

    report = _validate_capture_objects(captures, expected_version, len(capture_sources), expected_device_id, expected_ip)
    report["failures"] = source_failures + report["failures"]
    if len(capture_sources) >= 2:
        report["status"] = "passed" if not report["failures"] else "failed"
    return report


def validate_captures(captures, expected_version, expected_device_id=None, expected_ip=None):
    """Validate ordered paths containing already-captured status envelopes."""
    loaded = []
    failures = []
    for index, capture in enumerate(captures):
        try:
            path, digest, envelope = _load_capture(capture)
            loaded.append((path, digest, envelope, None))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            failures.append("capture_%d_%s" % (index, str(error)))
    report = _validate_capture_objects(loaded, expected_version, len(captures), expected_device_id, expected_ip)
    report["failures"] = failures + report["failures"]
    if len(captures) >= 2:
        report["status"] = "passed" if not report["failures"] else "failed"
    return report


def _validate_capture_objects(captures, expected_version, capture_count, expected_device_id=None, expected_ip=None):
    """Validate parsed envelopes and preserve source provenance when present."""
    report = {
        "status": "NOT_RUN",
        "device_ip": None,
        "expected_device_id": None,
        "identity_source": "explicit_argument" if expected_device_id is not None else "capture_metadata",
        "expected_version": expected_version,
        "capture_count": capture_count,
        "hardware_presence": "not_confirmed",
        "sensor_presence": "not_confirmed",
        "samples": [],
        "failures": [],
        "source_kind": "offline_captured_status_envelopes",
    }
    if capture_count < 2:
        report["failures"].append("at_least_two_ordered_captures_required")
        return report

    identity = expected_device_id
    device_ip = expected_ip
    for _path, _digest, envelope, provenance in captures:
        try:
            data = _status_data(envelope)
            if identity is None and data.get("model") == MODEL:
                identity = _device_identity(data.get("device_id", data.get("id")))
            if device_ip is None:
                device_ip = _capture_ip(envelope, provenance)
        except (ValueError, TypeError):
            continue
        if identity is not None and device_ip is not None:
            break
    try:
        identity = _device_identity(identity)
        device_ip = _device_ip(device_ip)
    except ValueError as error:
        report["failures"].append(str(error))
        report["status"] = "failed"
        return report
    report["expected_device_id"] = identity
    report["device_ip"] = device_ip
    previous_uptime = None
    previous_samples = None
    parsed = []
    for index, capture in enumerate(captures):
        try:
            path, digest, envelope, provenance = capture
            data = _status_data(envelope)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError,
                KeyError, TypeError) as error:
            report["failures"].append("capture_%d_%s" % (index, str(error)))
            continue

        identity_ok = (
            str(data.get("device_id", data.get("id", ""))).lower() == identity and
            data.get("model") == MODEL and data.get("version") == expected_version
        )
        if identity_ok:
            report["hardware_presence"] = "m1_identity_confirmed"
        else:
            report["failures"].append("capture_%d_identity_or_version_mismatch" % index)

        try:
            observed_ip = _capture_ip(envelope, provenance)
            if device_ip is not None and observed_ip != device_ip:
                report["failures"].append("capture_%d_device_ip_missing_or_mismatch" % index)
        except ValueError as error:
            report["failures"].append("capture_%d_%s" % (index, str(error)))

        barometer = data.get("barometer")
        if not isinstance(barometer, dict):
            report["sensor_presence"] = "sensor_not_present_in_response"
            report["failures"].append("capture_%d_barometer_missing" % index)
            continue

        model_ok = barometer.get("model") == BAROMETER_MODEL
        chip_ok = type(barometer.get("chip_id")) is int and barometer["chip_id"] == CHIP_ID
        address_ok = type(barometer.get("address")) is int and barometer["address"] == ADDRESS
        ready_ok = barometer.get("ready") is True and barometer.get("valid") is True
        age = barometer.get("age_ms")
        age_ok = type(age) is int and 0 <= age < MAX_AGE_MS
        pressure = barometer.get("pressure_hpa")
        pressure_ok = (not isinstance(pressure, bool) and
                       isinstance(pressure, (int, float)) and math.isfinite(pressure))
        sample_count = barometer.get("samples")
        samples_ok = type(sample_count) is int and sample_count >= 1
        uptime = data.get("uptime_seconds")
        uptime_ok = (not isinstance(uptime, bool) and
                     isinstance(uptime, (int, float)) and
                     math.isfinite(uptime) and uptime >= 0)

        if model_ok and chip_ok and address_ok:
            report["sensor_presence"] = "bmp388_hardware_confirmed"
        else:
            report["sensor_presence"] = "sensor_identity_mismatch_or_absent"
            report["failures"].append("capture_%d_not_expected_bmp388_at_0x77" % index)
        for condition, code in (
            (ready_ok, "not_ready_or_invalid"), (age_ok, "stale_or_invalid_age"),
            (pressure_ok, "pressure_missing_or_nonfinite"),
            (samples_ok, "sample_counter_invalid"), (uptime_ok, "uptime_invalid"),
        ):
            if not condition:
                report["failures"].append("capture_%d_%s" % (index, code))

        if uptime_ok and previous_uptime is not None and uptime < previous_uptime:
            report["failures"].append("capture_%d_uptime_regressed" % index)
        if samples_ok and previous_samples is not None and sample_count <= previous_samples:
            report["failures"].append("capture_%d_sensor_sample_counter_did_not_increase" % index)
        if uptime_ok:
            previous_uptime = uptime
        if samples_ok:
            previous_samples = sample_count
        row = {
            "index": index,
            "path": str(path),
            "sha256": digest,
            "identity_ok": identity_ok,
            "bmp388_chip80_address119": model_ok and chip_ok and address_ok,
            "ready_valid": ready_ok,
            "age_ms": age if age_ok else None,
            "pressure_finite": pressure_ok,
            "samples": sample_count if samples_ok else None,
            "uptime_seconds": uptime if uptime_ok else None,
        }
        if provenance is not None:
            row["provenance"] = provenance
            started = provenance.get("request_started_monotonic")
            ended = provenance.get("request_ended_monotonic")
            if (type(started) not in (int, float) or type(ended) not in (int, float) or
                    not math.isfinite(started) or not math.isfinite(ended) or ended < started):
                report["failures"].append("capture_%d_request_interval_invalid" % index)
            if index and report["samples"]:
                previous_provenance = report["samples"][-1].get("provenance", {})
                previous_end = previous_provenance.get("request_ended_monotonic")
                if (type(previous_end) in (int, float) and type(started) in (int, float) and
                        started - previous_end < 1.0):
                    report["failures"].append("capture_%d_too_close_to_previous_capture" % index)
        report["samples"].append(row)
        parsed.append(row)

    if len(parsed) != capture_count:
        report["hardware_presence"] = "not_confirmed" if not parsed else report["hardware_presence"]
        report["sensor_presence"] = "not_confirmed" if not parsed else report["sensor_presence"]
    report["status"] = "passed" if not report["failures"] else "failed"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--device-id", help="expected M1 identity; otherwise use the first capture identity")
    parser.add_argument("--device-ip", help="expected captured address; requires matching capture metadata")
    parser.add_argument("--capture", action="append", default=[],
                        help="ordered JSON status response envelope (repeat at stage boundaries)")
    parser.add_argument("--sources-json",
                        help="JSON array of existing evidence source descriptors for offline validation")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.capture and args.sources_json:
        parser.error("Use --capture or --sources-json, not both")
    if args.sources_json:
        report = validate_source_captures(json.loads(Path(args.sources_json).read_text()), args.version, args.device_id, args.device_ip)
    else:
        report = validate_captures(args.capture, args.version, args.device_id, args.device_ip)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    output.chmod(0o600)
    print(json.dumps({"status": report["status"], "failures": report["failures"]}, sort_keys=True))
    raise SystemExit(0 if report["status"] == "passed" else 1)


if __name__ == "__main__":
    main()
