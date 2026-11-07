#!/usr/bin/env python3
"""Verify an authorized BRY88 board running the community screen application.

Read-only by default. --exercise sends a short-lived external payload, pushes
and acknowledges an API notice, and creates/removes a one-shot reminder. It
does not reset, upload or install either processor. LCD appearance and touch
still require observation on the physical device.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import time
import zlib

from backup_screen_boot import Device


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--exercise", action="store_true")
    args = p.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    device = Device(args.url)
    report = args.evidence / "community-services.jsonl"
    expected_md5 = hashlib.md5(args.image.read_bytes()).hexdigest()
    with report.open("x") as log:
        def record(event, data):
            log.write(json.dumps({"time": time.time(), "event": event, "data": data},
                                 ensure_ascii=False) + "\n")
            log.flush()

        def command(action, params=None):
            data = device.command(action, params)
            record(action, data)
            return data

        def ready():
            status = command("status.get")
            screen = status["screen"]
            assert status["hardware_model"] == "BRY88BBA15G"
            assert screen["recovery"]["image_md5"] == expected_md5
            assert screen["recovery"]["available"]
            return status, (screen["recovery"]["mode"] == "normal"
                            and screen["extension_supported"]
                            and screen["capabilities"] & 13 == 13
                            and screen["layout_sync"] == "applied"
                            and screen["mcu_layout_revision"] == screen["layout_revision"])

        deadline = time.monotonic() + 90
        while True:
            before, ok = ready()
            if ok:
                break
            assert time.monotonic() < deadline, "Screen business startup/layout ACK unavailable"
            time.sleep(2)
        session = before["screen"]["recovery"]["session"]
        wire = bytearray()
        part = command("screen.layout.get", {"offset": 0, "limit": 512})
        total, revision, crc = part["total"], part["revision"], part["crc"]
        assert total == 9520
        for offset in range(0, total, 512):
            if offset:
                part = command("screen.layout.get", {"offset": offset, "limit": 512})
            assert part["offset"] == offset and part["total"] == total
            assert part["revision"] == revision and part["crc"] == crc
            wire.extend(base64.b64decode(part["bytes"], validate=True))
        assert len(wire) == total and wire[:4] == b"SCN3"
        stride = 288
        assert zlib.crc32(wire) == part["crc"]
        (args.evidence / "live-scene.bin").write_bytes(wire)
        weather = command("screen.weather.get")
        assert "key" not in weather["config"]
        rules = []
        for at in range(0, 32, 8):
            rules += command("screen.reminders.get", {"offset": at})["rules"]

        def notices():
            return [n for at in range(0, 32, 8)
                    for n in command("screen.reminders.get", {"offset": at})["notices"]]

        if args.exercise:
            for i in range(wire[9]):
                offset = 304 + i * stride
                if wire[offset + 4] == 6:
                    component = struct.unpack_from("<H", wire, offset)[0]
                    command("screen.data.push", {"id": component, "values": [410., 420., 405., 430.],
                                                  "text": "串口数据自检", "ttl": 5})
                    break
            notice = command("screen.reminders.push", {"title": "固件自检", "text": "API 提醒通道验证", "wake": 0})["id"]
            assert any(n["id"] == notice and n["unread"] for n in notices())
            command("screen.reminders.ack", {"id": notice})
            assert any(n["id"] == notice and not n["unread"] for n in notices())
            rule_id = next(i for i in range(0x7ffffffe, 0x7fffff00, -1)
                           if all(r["id"] != i for r in rules))
            rule = {"id": rule_id, "kind": 0, "epoch": int(time.time()) + 15,
                    "minute": 0, "days": 0, "wake": 0, "enabled": True,
                    "title": "固件自检", "text": "定时提醒通道验证"}
            command("screen.reminders.set", rule)
            try:
                deadline = time.monotonic() + 50
                while True:
                    fired = next((n for n in notices() if n["id"] == rule_id), None)
                    if fired:
                        assert fired["unread"]
                        command("screen.reminders.ack", {"id": rule_id})
                        break
                    assert time.monotonic() < deadline, "One-shot reminder did not fire; check ESP clock"
                    time.sleep(2)
            finally:
                command("screen.reminders.delete", {"id": rule_id})

        deadline = time.monotonic() + 60
        while True:
            after, ok = ready()
            assert after["screen"]["recovery"]["session"] == session, "MCU restarted"
            sensors = after["sensors"]
            if ok and all(sensors[k]["valid"] and sensors[k]["age_ms"] < 45000
                          for k in ("temperature", "humidity", "co2", "pm25", "tvoc")):
                break
            assert time.monotonic() < deadline, "Fresh sensor snapshots unavailable"
            time.sleep(2)
        assert sensors["tvoc"]["unit"] == "ppb"
        assert after["screen"]["last_rx_age_ms"] < 15000
        assert not after["screen"]["config_load_error"]
        record("PASS", {"session": session, "actual_internal_md5": expected_md5,
                        "exercise": args.exercise,
                        "limits": "Data/notice sends have no per-message MCU ACK; LCD/touch and real weather require separate evidence"})
        print("PASS community screen identity, maintenance, negotiated layout ACK and fresh sensors"
              + (", external payload, API and timed reminders" if args.exercise else ""))


if __name__ == "__main__":
    main()
