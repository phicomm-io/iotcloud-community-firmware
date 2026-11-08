#!/usr/bin/env python3
"""Reset a community screen into maintenance, back up its loader, and reinstall.

This is a destructive-to-the-running-session hardware acceptance test: it resets
the screen and installs the supplied application through ESP32/UART. Run only
on an authorized, externally powered development board. It preserves ESP32
configuration and refuses to replace any output directory. LCD/touch observation
and installation power-loss recovery are separate checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

from backup_screen_boot import Device, read_pass
from flash_screen import validate_image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--from-recovery", action="store_true",
                   help="Continue from the matching application already in forced maintenance")
    args = p.parse_args()
    args.image = args.image.resolve()
    image = validate_image(args.image)
    if b"install=factory-uart;mode=application;" not in image:
        p.error("Recovery acceptance requires a full application image")
    md5, sha = hashlib.md5(image).hexdigest(), hashlib.sha256(image).hexdigest()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    device = Device(args.url)
    with (args.output / "recovery-test.jsonl").open("x") as log:
        def record(event, data):
            item = {"time": time.time(), "event": event, "data": data}
            log.write(json.dumps(item, ensure_ascii=False) + "\n")
            log.flush()
            print(event, json.dumps(data, ensure_ascii=False), flush=True)

        before = device.command("status.get")
        s = before["screen"]
        if (before.get("hardware_model") != "BRY88BBA15G"
                or not s["recovery"]["available"]
                or s["recovery"]["mode"] != ("fault_recovery" if args.from_recovery else "normal")
                or s["recovery"]["image_md5"] != md5
                or (not args.from_recovery and
                    (not s["extension_supported"] or s["layout_sync"] != "applied"))):
            raise ValueError("Start with the matching full application in the requested mode")
        record("before", s)
        previous = s["recovery"]["session"]
        if args.from_recovery:
            recovery = s["recovery"]
            record("continued_existing_recovery", recovery)
        else:
            record("enter", device.command("screen.recovery.enter"))
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                recovery = device.command("screen.recovery.get")
                if (recovery["available"] and recovery["session"] != previous
                        and recovery["mode"] == "fault_recovery" and recovery["image_md5"] == md5):
                    break
                time.sleep(.3)
            else:
                raise TimeoutError("Full application failed to retain recovery across the factory loader")
        session, held_at = recovery["session"], time.monotonic()
        record("independent_recovery", recovery)
        request = device.command("screen.recovery.resume")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            rejected = device.command("screen.boot.result.get", {"request": request["request"]})
            if rejected["state"] == "mcu_rejected":
                assert rejected["result"] == 3 and rejected["session"] == session
                record("forced_resume_rejected", rejected)
                break
            time.sleep(.1)
        else:
            raise RuntimeError("Forced recovery accepted resume or lost its reply")
        backup = args.output / "boot-backup"
        backup.mkdir()
        reads = [read_pass(device, backup, n, session) for n in (1, 2)]
        assert reads[0]["sha256"] == reads[1]["sha256"], "Boot reads do not match"
        report = {"range_start": 0, "range_end_exclusive": 0x14000,
                  "mcu_session": session, "inherited_wdt": recovery["inherited_wdt"],
                  "reads": reads, "matching": True,
                  "method": "UART read-only 256-byte CRC chunks"}
        (backup / "boot-backup.json").write_text(json.dumps(report, indent=2) + "\n")
        while time.monotonic() - held_at < 35:
            time.sleep(1)
        recovery = device.command("screen.recovery.get")
        assert recovery["available"] and recovery["session"] == session
        assert recovery["mode"] == "fault_recovery" and recovery["boot_exported"] == 0x14000
        record("recovery_survives_normal_window", {"elapsed_seconds": time.monotonic() - held_at,
                                                    "recovery": recovery})
        subprocess.run([sys.executable, str(Path(__file__).with_name("flash_screen.py")),
                        "--url", args.url, "--image", str(args.image), "--mode", "install",
                        "--boot-backup", str(backup), "--log", str(args.output / "install.jsonl")],
                       check=True)
        deadline, observed_window = time.monotonic() + 90, False
        while time.monotonic() < deadline:
            status = device.command("status.get")
            screen = status["screen"]
            recovery = screen["recovery"]
            assert recovery["session"] != session and recovery["image_md5"] == md5
            assert recovery["available"]
            observed_window |= recovery["mode"] == "window"
            if (recovery["mode"] == "normal" and screen["extension_supported"]
                    and screen["layout_sync"] == "applied"):
                assert observed_window, "Startup maintenance window was not observed"
                assert screen["layout_revision"] == s["layout_revision"]
                record("PASS", {"running": screen, "startup_window_observed": True,
                                "sha256": sha, "actual_internal_md5": md5})
                break
            time.sleep(1)
        else:
            raise TimeoutError("Reinstalled screen did not resume normal layout service")


if __name__ == "__main__":
    main()
