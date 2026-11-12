#!/usr/bin/env python3
"""LAN motion webhook -> G3 MessagePack notifications. See examples/motion/."""
import argparse
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import threading

from backup_screen_boot import Device
from screen_media import operation


class MotionPush:
    def __init__(self, device, cameras):
        self.device, self.cameras = device, cameras
        self.ids = {}
        self.lock = threading.Lock()

    def event(self, camera, active):
        if camera not in self.cameras or type(active) is not bool:
            raise ValueError("Unknown camera or invalid motion state")
        # One MCU command receipt is shared by every camera: serialize through
        # confirmation, including queries that resolve a previous lost reply.
        with self.lock:
            if not active:
                return {"id": self.ids.get(camera), "state": "ending"}
            state = self.device.command("screen.push.get")
            if not state.get("policy_supported") or not state.get("update_supported"):
                raise RuntimeError("Update ESP32 and screen MCU first")
            ident = self.ids.get(camera)
            if ident and any(q["id"] == ident for q in state.get("queue", [])):
                result = operation(self.device, "screen.push.update",
                                   {"id": ident, "duration_ms": 15000, "ttl_ms": 15000})
            else:
                ident = secrets.randbelow(0xffffffff) + 1
                self.ids[camera] = ident  # Resolve this ID if the reply is lost.
                params = dict(self.cameras[camera])
                params.update(id=ident, mode="timed", duration_ms=15000, ttl_ms=15000,
                              overflow="drop_oldest", wake=True)
                result = operation(self.device, "screen.push.send", params)
                # Only send can merge into another canonical URL ID. An update
                # leaves last_push_id pointing at the most recent other send.
                ident = result.get("last_push_id", ident)
            self.ids[camera] = ident
            status = next((q["state"] for q in result.get("queue", []) if q["id"] == ident),
                          next((q["state"] for q in result.get("messages", []) if q["id"] == ident), "ended"))
            return {"id": ident, "state": status}


def serve(device, cameras, token, host, port):
    bridge = MotionPush(device, cameras)

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != "/motion":
                return self.reply(404, {"error": "not_found"})
            if not hmac.compare_digest(self.headers.get("Authorization", "").encode("utf-8"), ("Bearer " + token).encode()):
                return self.reply(401, {"error": "unauthorized"})
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 4096:
                    raise ValueError("Invalid body size")
                self.connection.settimeout(5)
                event = json.loads(self.rfile.read(size))
                if not isinstance(event, dict) or set(event) != {"camera", "active"} or not isinstance(event["camera"], str):
                    raise ValueError("Expected camera and active")
                result = bridge.event(event["camera"], event["active"])
            except (ValueError, TypeError):
                return self.reply(400, {"error": "invalid_event"})
            except Exception as error:
                # URLs may contain private credentials; do not log exception text.
                print("Motion push failed:", type(error).__name__, flush=True)
                return self.reply(503, {"error": "device_unconfirmed"})
            self.reply(200, result)

        def reply(self, status, data):
            blob = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(blob)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(blob)
            self.close_connection = True

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--cameras", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    token = os.environ.get("G3_MOTION_TOKEN", "")
    if len(token) < 24 or not token.isascii():
        parser.error("Set G3_MOTION_TOKEN to at least 24 ASCII characters")
    cameras = json.loads(args.cameras.read_text())
    if not isinstance(cameras, dict) or not cameras:
        parser.error("Camera map must be a non-empty object")
    serve(Device(args.url), cameras, token, args.host, args.port)
