#!/usr/bin/env python3
"""Read the screen MCU's first 80 KiB twice through authenticated ESP commands.

Requires a community screen MCU exposing screen_recovery v3, plus msgpack.
Never writes MCU flash, changes protection, activates OTA or resets either chip.
--hold pauses the MCU startup window; --resume releases that hold after success.
Set IOTCLOUD_AUTH=user:password if the ESP has an administration password.
"""
import argparse
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import time
import urllib.request
import urllib.error
import uuid
import zlib

import msgpack

TOTAL = 0x14000
CHUNK = 256


class Device:
    def __init__(self, url):
        self.url = url.rstrip("/")
        self.headers = {"Content-Type": "application/msgpack"}
        auth = os.environ.get("IOTCLOUD_AUTH")
        if auth:
            self.headers["Authorization"] = "Basic " + base64.b64encode(auth.encode()).decode()

    def command(self, action, params=None):
        query = action in ("status.get", "screen.recovery.get", "screen.boot.result.get")
        body = msgpack.packb({"request_id": uuid.uuid4().hex, "action": action,
                             "params": params or {}}, use_bin_type=True)
        req = urllib.request.Request(self.url + "/api/v1/command", body, self.headers)
        for attempt in range(5):
            try:
                with urllib.request.urlopen(req, timeout=8) as r:
                    if r.status != 200:
                        raise RuntimeError(f"HTTP {r.status}")
                    reply = msgpack.unpackb(r.read(), raw=False)
                break
            except urllib.error.HTTPError as error:
                # Only retry queries; never duplicate a stateful UART command.
                if error.code != 503 or attempt == 4 or not query:
                    raise
                time.sleep(.1 * (attempt + 1))
            except (urllib.error.URLError, TimeoutError, ConnectionError,
                    http.client.HTTPException):
                if not query or attempt == 4:
                    raise
                time.sleep(.1 * (attempt + 1))
        if reply.get("code") not in ("ok", "accepted"):
            raise RuntimeError(f"{action}: {reply.get('code')}")
        return reply.get("data", {})

    def wait_result(self, request, session):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            data = self.command("screen.boot.result.get", {"request": request})
            if not data.get("available") or data.get("session") != session:
                raise RuntimeError("MCU session changed or recovery service disappeared")
            if data.get("state") == "complete":
                if data.get("result") != 0:
                    raise RuntimeError("MCU rejected request")
                return data
            if data.get("state") != "pending":
                raise RuntimeError(f"MCU request: {data.get('state')}")
            time.sleep(.025)
        raise TimeoutError("MCU response timeout")

    def operation(self, action):
        req = self.command(action)
        return self.wait_result(req["request"], req["session"])


def read_pass(device, directory, number, session, interval=0):
    partial = directory / f"boot-read{number}.partial"
    final = directory / f"boot-read{number}.bin"
    if partial.exists() or final.exists():
        raise FileExistsError(f"Refusing to replace {partial} or {final}")
    digest = hashlib.sha256()
    with partial.open("xb") as out:
        for offset in range(0, TOTAL, CHUNK):
            if interval:
                time.sleep(interval)
            req = device.command("screen.boot.read", {"offset": offset, "length": CHUNK})
            if req.get("session") != session:
                raise RuntimeError("MCU restarted between requests")
            result = device.wait_result(req["request"], session)
            raw = base64.b64decode(result.get("bytes", ""), validate=True)
            if (result.get("offset") != offset or result.get("total") != TOTAL or
                    result.get("length") != CHUNK or len(raw) != CHUNK or
                    result.get("crc32") != zlib.crc32(raw)):
                raise ValueError(f"Invalid data or CRC at {offset:#x}")
            out.write(raw)
            digest.update(raw)
            if (offset + CHUNK) % 4096 == 0:
                print(f"pass {number}: {offset + CHUNK}/{TOTAL}", flush=True)
        out.flush()
        os.fsync(out.fileno())
    partial.rename(final)
    data = final.read_bytes()
    if len(data) != TOTAL or data == b"\xff" * TOTAL or data == b"\0" * TOTAL:
        raise ValueError("Empty or incomplete boot region")
    return {"file": final.name, "size": len(data), "sha256": digest.hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--hold", action="store_true")
    p.add_argument("--resume", action="store_true")
    p.add_argument("--interval-ms", type=int, default=0,
                   help="Pause before each 256-byte read (0..1000 ms)")
    args = p.parse_args()
    if not 0 <= args.interval_ms <= 1000:
        p.error("--interval-ms must be 0..1000")
    if args.resume and not args.hold:
        p.error("--resume requires --hold")
    args.output.mkdir(parents=True, exist_ok=True)
    report = args.output / "boot-backup.json"
    if report.exists():
        raise FileExistsError(report)
    device = Device(args.url)
    status = device.command("status.get")
    if status.get("hardware_model") != "BRY88BBA15G":
        raise ValueError("Unexpected ESP hardware model")
    before = device.command("screen.recovery.get")
    if not before.get("available") or not before.get("session"):
        raise RuntimeError("Community MCU recovery service unavailable; stock MCU cannot export boot flash")
    if args.hold:
        held = device.operation("screen.recovery.hold")
        if held["session"] != before["session"]:
            raise RuntimeError("MCU session changed before hold")
    reads = [read_pass(device, args.output, n, before["session"],
                      args.interval_ms / 1000) for n in (1, 2)]
    if reads[0]["sha256"] != reads[1]["sha256"]:
        raise ValueError("The two full reads differ; retained both, backup is not reliable")
    if args.resume:
        device.operation("screen.recovery.resume")
    result = {"range_start": 0, "range_end_exclusive": TOTAL,
              "mcu_session": before["session"], "inherited_wdt": before.get("inherited_wdt"),
              "reads": reads, "matching": True, "interval_ms": args.interval_ms,
              "method": "UART read-only 256-byte CRC chunks"}
    with report.open("x") as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(f"Verified two identical 80 KiB reads: {reads[0]['sha256']}")


if __name__ == "__main__":
    main()
