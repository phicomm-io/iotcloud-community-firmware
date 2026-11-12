#!/usr/bin/env python3
"""Exercise an authorized BRY88 development board; never activate MCU OTA.

Requires msgpack. With --exercise this saves a new revision of the same layout,
adds/deletes one disabled reminder, acknowledges one API notice, uploads the MCU
artifact to ESP storage, checks the factory-MCU install gate, and reboots ESP.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import time
import urllib.error
import urllib.request
import uuid
import zlib

import msgpack


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--mcu-image", type=Path)
    p.add_argument("--exercise", action="store_true")
    args = p.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    events = []

    def save():
        (args.evidence / "live-services.json").write_text(
            json.dumps(events, ensure_ascii=False, indent=2) + "\n"
        )

    def request(path, data=None, headers=None, timeout=15):
        req = urllib.request.Request(args.url.rstrip("/") + path, data, headers or {})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                code, raw = r.status, r.read()
        except urllib.error.HTTPError as r:
            code, raw = r.code, r.read()
        return code, msgpack.unpackb(raw, raw=False)

    def command(action, params=None, expected="ok"):
        body = {"request_id": uuid.uuid4().hex, "action": action, "params": params or {}}
        status, reply = request("/api/v1/command", msgpack.packb(body, use_bin_type=True),
                                {"Content-Type": "application/msgpack"})
        events.append({"action": action, "params": params or {}, "http": status, "reply": reply})
        save()
        assert status == 200 and reply["code"] == expected, (action, status, reply)
        return reply.get("data", {})

    def status():
        http, reply = request("/api/v1/status")
        assert http == 200 and reply["code"] == "ok"
        d = reply["data"]
        assert d["hardware_model"] == "BRY88BBA15G"
        return d

    def layout():
        wire = bytearray()
        for at in range(0, 9520, 512):
            part = command("screen.layout.get", {"offset": at, "limit": 512})
            assert part["total"] == 9520 and part["offset"] == at
            wire += base64.b64decode(part["bytes"], validate=True)
        assert len(wire) == 9520 and wire[:4] == b"SCN3"
        assert zlib.crc32(wire) == part["crc"]
        return wire

    before = status()
    deadline = time.monotonic() + 40
    while not before["screen"]["mcu_version"]:
        assert time.monotonic() < deadline, "Stock MCU version exchange did not resume"
        time.sleep(2)
        before = status()
    events.append({"checkpoint": "before", "status": before})
    assert not before["screen"]["extension_supported"], "This acceptance scenario expects stock MCU"
    mcu_version = before["screen"]["mcu_version"]
    wire = layout()
    command("screen.layout.get", {"offset": 9520, "limit": 512}, "invalid_params")
    command("screen.reminders.get", {"offset": 8})
    command("screen.reminders.get", {"offset": 32}, "invalid_params")
    command("screen.weather.get")
    command("screen.weather.set", {"enabled": True, "provider": "unknown"}, "invalid_params")
    command("screen.update.get")
    if not args.exercise:
        save()
        print("PASS read-only MessagePack queries, pagination and invalid-input checks")
        return

    revision = struct.unpack_from("<I", wire, 4)[0] + 1
    struct.pack_into("<I", wire, 4, revision)
    command("screen.layout.begin", {"revision": revision, "total": len(wire), "crc": zlib.crc32(wire)})
    for at in range(0, len(wire), 512):
        command("screen.layout.chunk", {"offset": at, "bytes": base64.b64encode(wire[at:at + 512]).decode()})
    command("screen.layout.commit", {"revision": revision}, "accepted")
    assert layout() == wire
    (args.evidence / "live-scene.bin").write_bytes(wire)
    rules = []
    for at in range(0, 32, 8):
        rules += command("screen.reminders.get", {"offset": at})["rules"]
    rule_id = next(i for i in range(0x7ffffffe, 0x7fffff00, -1) if all(r["id"] != i for r in rules))
    rule = {"id": rule_id, "kind": 2, "epoch": 0, "minute": 600, "days": 17,
            "wake": 0, "enabled": False, "title": "接口验证", "text": "禁用的测试规则"}
    command("screen.reminders.set", rule)
    notice = command("screen.reminders.push", {"title": "接口验证", "text": "串口兼容及存储验证", "wake": 0}, "accepted")["id"]
    command("screen.reminders.ack", {"id": notice})
    if args.mcu_image:
        image = args.mcu_image.read_bytes()
        assert len(image) == 0x67000 and image[:4] == struct.pack("<I", 0x20010000)
        headers = {"Content-Type": "application/octet-stream", "X-IotCloud-Target": "BRY88BBA15G-screen"}
        http, reply = request("/api/v1/screen/ota/upload", image[:64], headers)
        events.append({"checkpoint": "short_mcu_upload", "http": http, "reply": reply})
        save()
        assert http == 400 and reply["code"] == "invalid_firmware"
        http, reply = request("/api/v1/screen/ota/upload", image, headers, 55)
        events.append({"checkpoint": "mcu_artifact_upload", "http": http, "reply": reply})
        save()
        assert http == 200 and reply["code"] == "ok"
        assert reply["data"]["sha256"] == hashlib.sha256(image).hexdigest()
        assert reply["data"]["md5"] == hashlib.md5(image).hexdigest()
        assert reply["data"]["installable"] is False
        command("screen.update.start", {"mode": "stage"}, "bootloader_unverified")
        command("screen.update.install", {}, "bootloader_unverified")

    command("system.reboot", {}, "accepted")
    deadline = time.monotonic() + 55
    while True:
        time.sleep(2)
        try:
            after = status()
            if after["snapshot_uptime_ms"] < before["snapshot_uptime_ms"]:
                break
        except (OSError, ValueError):
            pass
        assert time.monotonic() < deadline, "ESP did not return after reboot"
    assert layout() == wire
    restored = []
    for at in range(0, 32, 8):
        restored += command("screen.reminders.get", {"offset": at})["rules"]
    assert any(r["id"] == rule_id and r["enabled"] is False and r["days"] == 17 for r in restored)
    command("screen.reminders.delete", {"id": rule_id})
    deadline = time.monotonic() + 40
    while True:
        after = status()
        s = after["screen"]
        if s["mcu_version"] == mcu_version and s["uart_frames"] > 1 and after["sensors"]["co2"]["valid"]:
            break
        assert time.monotonic() < deadline, "Stock MCU snapshots did not resume"
        time.sleep(2)
    assert s["extension_supported"] is False and s["layout_sync"] == "waiting"
    assert s["mcu_layout_revision"] == 0 and s["storage"] == "owned"
    events.append({"checkpoint": "after_reboot_and_cleanup", "status": after})
    save()
    print("PASS live ESP: paginated SCN3, durable layout/rules, API notices, checked artifact upload, MCU gate, reboot and stock-MCU sensors")


if __name__ == "__main__":
    main()
