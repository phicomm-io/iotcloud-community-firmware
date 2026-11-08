#!/usr/bin/env python3
"""Upload and test a BRY88BBA15G screen application through its ESP32.

install sends a complete application through the stock or community UART path.
Boot backups are optional; --reuse-boot-backup retains their original session.
Image validation, staging readback and the community MCU's copy-tail checks
remain active. The original boot region is not rewritten.
IOTCLOUD_AUTH optionally supplies the existing HTTP administration credentials.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import urllib.request
import urllib.error

import msgpack
from backup_screen_boot import Device

def validate_image(image):
    data = image.read_bytes()
    if len(data) != 0x67000:
        raise ValueError("Expected the 421888-byte application, not a full flash image")
    sp, reset = struct.unpack_from("<II", data)
    if sp != 0x20010000 or not reset & 1 or not 0x14000 < reset < 0x7b000:
        raise ValueError("Invalid application vectors")
    if b"iotcloud-screen/BRY88BBA15G;application=0x14000;install=factory-uart;" not in data:
        raise ValueError("Wrong target or missing UART installation service")
    return data


def validate_backup(directory, session, reuse=False):
    report = json.loads((directory / "boot-backup.json").read_text())
    if not report.get("matching") or (not reuse and report.get("mcu_session") != session):
        raise ValueError("Boot backup must match this running MCU session")
    digests = []
    for n in (1, 2):
        data = (directory / f"boot-read{n}.bin").read_bytes()
        if len(data) != 0x14000 or data == b"\xff" * len(data) or data == b"\0" * len(data):
            raise ValueError("Empty/incomplete boot backup")
        digests.append(hashlib.sha256(data).hexdigest())
    if (len(report.get("reads", [])) != 2 or digests[0] != digests[1]
            or any(r["sha256"] != digests[0] for r in report["reads"])):
        raise ValueError("Boot backup digest mismatch")
    return digests[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--mode", choices=("stage", "factory-bootstrap", "install"), required=True)
    p.add_argument("--boot-backup", type=Path, help="Optionally verify an existing pair of boot backups")
    p.add_argument("--reuse-boot-backup", action="store_true",
                   help="Reuse a verified backup across application resets without changing its report")
    p.add_argument("--log", type=Path, required=True)
    p.add_argument("--upload-interval-ms", type=int, default=0,
                   help="Pause per uploaded KiB (0..1000 ms)")
    args = p.parse_args()
    if not 0 <= args.upload_interval_ms <= 1000:
        p.error("--upload-interval-ms must be 0..1000")
    if args.reuse_boot_backup and not args.boot_backup:
        p.error("--reuse-boot-backup requires --boot-backup")
    image = validate_image(args.image)
    sha, md5 = hashlib.sha256(image).hexdigest(), hashlib.md5(image).hexdigest()
    device = Device(args.url)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("x") as log:
        def record(event, data):
            item = {"time": time.time(), "event": event, "data": data}
            log.write(json.dumps(item, ensure_ascii=False) + "\n")
            log.flush()
            print(json.dumps(item, ensure_ascii=False), flush=True)
        status = device.command("status.get")
        if status.get("hardware_model") != "BRY88BBA15G":
            raise ValueError("Unexpected hardware model")
        recovery = device.command("screen.recovery.get")
        factory = not recovery.get("available")
        if args.mode == "factory-bootstrap" and not factory:
            raise ValueError("MCU already has community maintenance; use --mode install")
        if factory and args.mode == "stage":
            raise ValueError("Stock MCU auto-installs; use --mode install for direct installation")
        if not factory and args.mode == "install" and not recovery.get("ota_activation"):
            raise ValueError("MCU installation service unavailable")
        if args.boot_backup:
            backup_sha = validate_backup(args.boot_backup, recovery.get("session"), args.reuse_boot_backup)
            record("boot_backup", {"directory": str(args.boot_backup.resolve()),
                                   "reused": args.reuse_boot_backup,
                                   "sha256": backup_sha,
                                   "mcu_session": recovery.get("session")})
        record("installation_path", {"transport": "factory-uart" if factory else "community-maintenance",
                                     "mcu_version": status.get("screen", {}).get("mcu_version"),
                                     "boot_backup_required": False})
        headers = dict(device.headers, **{"Content-Type": "application/octet-stream",
                                        "X-IotCloud-Target": "BRY88BBA15G-screen"})
        def upload_chunks():
            for at in range(0, len(image), 1024):
                yield image[at:at + 1024]
                time.sleep(args.upload_interval_ms / 1000)
        headers["Content-Length"] = str(len(image))
        body = upload_chunks() if args.upload_interval_ms else image
        req = urllib.request.Request(device.url + "/api/v1/screen/ota/upload", body, headers)
        with urllib.request.urlopen(req, timeout=30) as response:
            result = msgpack.unpackb(response.read(), raw=False)
        record("upload", result)
        if result.get("code") != "ok" or result.get("data", {}).get("sha256") != sha:
            raise ValueError("ESP image readback/digest verification failed")
        params = {"mode": "factory-bootstrap" if factory else "stage"}
        if factory: params["sha256"] = sha
        record("start", device.command("screen.update.start", params))
        deadline, last = time.monotonic() + 600, None
        while time.monotonic() < deadline:
            try:
                status = device.command("status.get")
            except (urllib.error.URLError, TimeoutError) as error:
                record("status_retry", {"error": str(error)})
                time.sleep(.5)
                continue
            screen = status["screen"]
            ota, recovery = screen["ota"], screen["recovery"]
            progress = (ota["state"], ota["sent"], recovery.get("session"),
                        recovery.get("image_md5"), recovery.get("mode"))
            if progress != last:
                record("progress", {"ota": ota, "recovery": recovery})
                last = progress
            if factory:
                if recovery.get("available") and recovery.get("image_md5") == md5:
                    if recovery.get("mode") == "fault_recovery":
                        raise RuntimeError(f"Installed MCU failed to start normally: {recovery}")
                    if recovery.get("mode") == "normal":
                        record("verified_running", recovery)
                        return
                if ota["state"] == "artifact_ready":
                    raise RuntimeError("ESP restarted before the MCU completed its transfer")
            elif ota["state"] == "staged_verified":
                break
            if ota["state"] in ("timeout", "read_error", "write_error", "cancelled", "upload_failed"):
                raise RuntimeError(ota["state"])
            time.sleep(.4)
        else:
            raise TimeoutError("MCU did not complete the requested OTA phase")
        if args.mode == "stage":
            record("staged", {"sha256": sha})
            return
        previous_session = recovery.get("session")
        record("install", device.command("screen.update.install", {"sha256": sha}))
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                recovery = device.command("screen.recovery.get")
            except (urllib.error.URLError, TimeoutError):
                time.sleep(.5)
                continue
            if (recovery.get("available") and recovery.get("session") != previous_session
                    and recovery.get("image_md5") == md5):
                if recovery.get("mode") == "fault_recovery":
                    raise RuntimeError(f"Installed MCU failed to start normally: {recovery}")
                if recovery.get("mode") == "normal":
                    record("verified_running", recovery)
                    return
            if recovery.get("state") in ("mcu_rejected", "invalid_response", "timeout"):
                raise RuntimeError(f"MCU installation rejected: {recovery}")
            time.sleep(.4)
        raise TimeoutError("New MCU image did not announce its matching internal-flash digest")


if __name__ == "__main__":
    main()
