#!/usr/bin/env python3
"""Prepare images and call the G3 MessagePack media API (msgpack, Pillow).

IOTCLOUD_AUTH optionally supplies admin:password for the existing HTTP API.
Camera passwords are read from G3_CAMERA_PASSWORD, never command-line arguments.
Images, URL sources and notifications are transient. Camera configuration survives ESP reboot.
"""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import secrets
import time
import zlib

from backup_screen_boot import Device


def prepare(source, width=320, height=240):
    from PIL import Image, ImageOps
    if not 1 <= width <= 480 or not 1 <= height <= 854:
        raise ValueError("Resolution must be 1..480 by 1..854")
    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image = ImageOps.pad(image, (width, height), color="black")
        for quality in (85, 70, 55, 40, 25):
            output = io.BytesIO()
            image.save(output, format="JPEG", quality=quality, progressive=False,
                       subsampling=2, optimize=False)
            data = output.getvalue()
            if len(data) <= 131072:
                return data
    raise ValueError("Image exceeds 128 KiB; choose a smaller resolution")


def operation(device, action, params=None):
    receipt = device.command(action, params)
    sequence = receipt.get("request")
    if sequence is None:
        raise RuntimeError(f"{action}: missing MCU receipt")
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        state = device.command("screen.media.get")
        request = state.get("request", {})
        if request.get("sequence") != sequence:
            raise RuntimeError("Another client replaced the current request; query media state")
        if request.get("state") == "complete" and request.get("result") == 0:
            return state
        if request.get("state") in ("uncertain", "rejected"):
            raise RuntimeError(f"{action}: {request['state']} ({request.get('result')})")
        time.sleep(.08)
    raise TimeoutError("MCU receipt timeout; query state before retrying")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", help="G3 HTTP origin, e.g. http://device")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "push"):
        p = sub.add_parser(name)
        p.add_argument("--image", type=Path, required=name == "prepare")
        p.add_argument("--width", type=int, default=320)
        p.add_argument("--height", type=int, default=240)
        if name == "prepare":
            p.add_argument("--output", type=Path, required=True)
        else:
            p.add_argument("--source", help="HTTP(S) JPEG/MJPEG URL (exclusive with --image)")
            p.add_argument("--username", default="")
            p.add_argument("--fps", type=int, default=1)
            p.add_argument("--insecure", action="store_true")
            p.add_argument("--id", type=int, default=secrets.randbelow(0xffffffff) + 1)
            p.add_argument("--title", default="")
            p.add_argument("--text", default="")
            p.add_argument("--mode", choices=("timed", "confirm"), default="timed")
            p.add_argument("--seconds", type=int, default=10)
            p.add_argument("--ttl", type=int, help="Expire this many seconds after receipt, even while hidden; 0 disables")
            p.add_argument("--overflow", choices=("reject", "drop_newest", "drop_oldest"))
            p.add_argument("--no-wake", action="store_true")
            p.add_argument("--brightness", type=int, default=30)
    p = sub.add_parser("update", help="Patch a queued notification without moving its position")
    p.add_argument("id", type=int)
    media = p.add_mutually_exclusive_group()
    media.add_argument("--image", type=Path)
    media.add_argument("--source", help="Replace URL; empty string removes media")
    media.add_argument("--clear-media", action="store_true")
    for key in ("title", "text", "username"):
        p.add_argument("--" + key)
    for key in ("width", "height", "fps", "brightness", "seconds"):
        p.add_argument("--" + key, type=int)
    p.add_argument("--ttl", type=int, help="Reset expiry in seconds; 0 disables")
    p.add_argument("--mode", choices=("timed", "confirm"))
    p.add_argument("--wake", action=argparse.BooleanOptionalAction, default=None)
    p.add_argument("--verify-tls", action=argparse.BooleanOptionalAction, default=None)
    p.add_argument("--password-env", action="store_true", help="Patch password from G3_CAMERA_PASSWORD (empty clears it)")
    p = sub.add_parser("get")
    p.add_argument("kind", choices=("media", "push", "stream", "link"), default="media", nargs="?")
    p = sub.add_parser("dismiss")
    p.add_argument("id", type=int)
    p = sub.add_parser("select")
    p.add_argument("index", type=int, choices=range(5), help="0: normal page; 1..4: notification stack")
    p = sub.add_parser("link")
    p.add_argument("--mode", choices=("fixed", "auto"), default="fixed")
    p.add_argument("--rate", type=int, choices=(115200, 230400, 460800, 921600), default=115200)
    p.add_argument("--max-rate", type=int, choices=(115200, 230400, 460800, 921600), default=921600)
    p.add_argument("--apply-now", action="store_true", help="Restart screen MCU to negotiate in recovery")
    p = sub.add_parser("stream")
    p.add_argument("--source", required=True, help="HTTP(S) MJPEG URL; accepts {width}/{height}/{fps}")
    p.add_argument("--username", default="")
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--fps", type=int, default=1)
    p.add_argument("--insecure", action="store_true", help="Allow a source with a self-signed TLS certificate")
    p.add_argument("--disabled", action="store_true")
    sub.add_parser("start")
    sub.add_parser("stop")
    args = parser.parse_args()
    if args.command == "prepare":
        data = prepare(args.image, args.width, args.height)
        args.output.write_bytes(data)
        print(json.dumps({"file": str(args.output), "total": len(data),
                          "crc32": zlib.crc32(data), "width": args.width, "height": args.height}))
        return
    if not args.url:
        parser.error("--url is required for device commands")
    device = Device(args.url)
    if args.command == "get":
        result = device.command(f"screen.{args.kind}.get")
    elif args.command == "select":
        result = operation(device, "screen.push.select", {"index": args.index})
    elif args.command == "link":
        result = device.command("screen.link.set", {"mode": args.mode, "rate": args.rate,
                                "max_rate": args.max_rate, "apply_now": args.apply_now})
    elif args.command == "stream":
        params = {"url": args.source, "username": args.username, "width": args.width,
                  "height": args.height, "fps": args.fps, "verify_tls": not args.insecure,
                  "enabled": not args.disabled}
        if "G3_CAMERA_PASSWORD" in os.environ:
            params["password"] = os.environ["G3_CAMERA_PASSWORD"]
        result = operation(device, "screen.stream.set", params)
    elif args.command in ("start", "stop"):
        result = operation(device, "screen.stream." + args.command)
    elif args.command == "update":
        if not 1 <= args.id <= 0xffffffff:
            parser.error("ID must be a nonzero uint32")
        params = {"id": args.id}
        for key in ("title", "text", "username", "fps", "brightness", "mode", "wake", "verify_tls"):
            value = getattr(args, key)
            if value is not None:
                params[key] = value
        if args.ttl is not None:
            if not 0 <= args.ttl <= 86400: parser.error("TTL must be 0..86400 seconds")
            params["ttl_ms"] = args.ttl * 1000
        if args.seconds is not None:
            params["duration_ms"] = args.seconds * 1000
        if args.source is not None:
            params["url"] = args.source
        if not args.image:
            for key in ("width", "height"):
                if getattr(args, key) is not None:
                    params[key] = getattr(args, key)
        if args.password_env or (args.source and "G3_CAMERA_PASSWORD" in os.environ):
            if "G3_CAMERA_PASSWORD" not in os.environ:
                parser.error("Set G3_CAMERA_PASSWORD before --password-env")
            params["password"] = os.environ["G3_CAMERA_PASSWORD"]
        if args.clear_media:
            params["image_id"] = 0
        image_id = 0
        try:
            if args.image:
                data = prepare(args.image, args.width or 320, args.height or 240)
                image_id = secrets.randbelow(0xffffffff) + 1
                operation(device, "screen.media.begin", {"id": image_id, "total": len(data),
                          "crc32": zlib.crc32(data), "width": args.width or 320, "height": args.height or 240})
                for offset in range(0, len(data), 512):
                    operation(device, "screen.media.chunk", {"id": image_id, "offset": offset,
                              "bytes": base64.b64encode(data[offset:offset+512]).decode()})
                operation(device, "screen.media.commit", {"id": image_id})
                params["image_id"] = image_id
            if len(params) < 2:
                parser.error("Supply at least one field to update")
            result = operation(device, "screen.push.update", params)
        except Exception:
            if image_id:
                try:
                    operation(device, "screen.media.cancel", {"id": image_id})
                except Exception:
                    pass
            raise
    elif args.command == "dismiss":
        result = operation(device, "screen.push.dismiss", {"id": args.id})
    else:
        if (not 1 <= args.id <= 0xffffffff or len(args.title.encode()) > 64 or
                len(args.text.encode()) > 512 or not 1 <= args.seconds <= 3600 or
                not 1 <= args.brightness <= 100 or not (args.image or args.source or args.title or args.text) or
                (args.image and args.source) or not 1 <= args.fps <= 10):
            parser.error("Invalid push id, text, duration or brightness")
        image_id = 0
        try:
            if args.image:
                data = prepare(args.image, args.width, args.height)
                image_id = args.id
                operation(device, "screen.media.begin", {"id": image_id, "total": len(data),
                          "crc32": zlib.crc32(data), "width": args.width, "height": args.height})
                for offset in range(0, len(data), 512):
                    operation(device, "screen.media.chunk", {"id": image_id, "offset": offset,
                              "bytes": base64.b64encode(data[offset:offset+512]).decode()})
                operation(device, "screen.media.commit", {"id": image_id})
            params = {"id": args.id, "image_id": image_id,
                               "title": args.title, "text": args.text, "mode": args.mode,
                               "duration_ms": args.seconds * 1000, "wake": not args.no_wake,
                               "brightness": args.brightness}
            if args.ttl is not None:
                if not 0 <= args.ttl <= 86400: parser.error("TTL must be 0..86400 seconds")
                params["ttl_ms"] = args.ttl * 1000
            if args.overflow is not None: params["overflow"] = args.overflow
            if args.source:
                params.update(url=args.source, username=args.username, width=args.width,
                              height=args.height, fps=args.fps, verify_tls=not args.insecure)
                if "G3_CAMERA_PASSWORD" in os.environ:
                    params["password"] = os.environ["G3_CAMERA_PASSWORD"]
            result = operation(device, "screen.push.send", params)
        except Exception:
            if image_id:
                try:
                    operation(device, "screen.media.cancel", {"id": image_id})
                except Exception:
                    pass
            raise
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
