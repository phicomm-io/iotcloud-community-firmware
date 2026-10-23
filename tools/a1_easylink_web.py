#!/usr/bin/env python3
"""Loopback-only browser interface for the A1 stock EasyLink sender (stdlib)."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import secrets
import threading
import time
import webbrowser

from a1_easylink import data_packets, encode, interface_ipv4, send


ASSETS = Path(__file__).with_name("a1_easylink_web")


class Provisioning:
    def __init__(self, sender=send):
        self.sender = sender
        self.lock = threading.Lock()
        self.cancel = threading.Event()
        self.worker = None
        self.started = 0
        self.state = {"phase": "idle", "count": 0, "cycles": 0,
                      "duration": 50, "message": "尚未开始发送。"}

    def snapshot(self):
        with self.lock:
            state = dict(self.state)
            state["elapsed"] = round(time.monotonic() - self.started, 1) if self.started else 0
            if state["phase"] != "sending":
                state["elapsed"] = state.get("finished_elapsed", 0)
            return state

    def start(self, request):
        if not isinstance(request, dict):
            raise ValueError("请求应为 JSON 对象")
        for key in ("ssid", "password", "interface_ip"):
            if not isinstance(request.get(key), str):
                raise ValueError("请填写 Wi-Fi 名称、密码和无线网卡 IPv4 地址")
        if not isinstance(request.get("open_network"), bool):
            raise ValueError("请选择网络是否有密码")
        if request["open_network"]:
            if request["password"]:
                raise ValueError("开放网络请清空密码")
        elif not request["password"]:
            raise ValueError("请填写密码，或明确选择开放网络")
        address = str(interface_ipv4(request["interface_ip"]))
        packets = data_packets(encode(request["ssid"], request["password"], address))
        try:
            duration = float(request.get("duration", 50))
            interval_ms = float(request.get("interval_ms", 10))
        except (TypeError, ValueError, OverflowError):
            raise ValueError("发送时长和包间隔须为数字") from None
        if not math.isfinite(duration) or not 1 <= duration <= 60:
            raise ValueError("发送时长须在 1–60 秒之间")
        if not math.isfinite(interval_ms) or not 1 <= interval_ms <= 200:
            raise ValueError("包间隔须在 1–200 毫秒之间")
        with self.lock:
            if self.worker is not None and self.worker.is_alive():
                raise RuntimeError("已有发送任务，请先停止或等待结束")
            self.cancel = threading.Event()
            self.started = time.monotonic()
            self.state = {"phase": "sending", "count": 0, "cycles": 0,
                          "duration": duration, "message": "正在发送；请让 A1 进入配网模式。"}
            self.worker = threading.Thread(target=self._run,
                                           args=(packets, address, duration, interval_ms / 1000),
                                           daemon=True)
            self.worker.start()

    def _progress(self, count, cycles, elapsed):
        with self.lock:
            self.state.update(count=count, cycles=cycles)

    def _run(self, packets, address, duration, interval):
        try:
            count, cycles = self.sender(packets, address, duration=duration, interval=interval,
                                       progress=self._progress, cancel=self.cancel)
            phase = "stopped" if self.cancel.is_set() else "sent"
            message = ("已停止发送。" if phase == "stopped" else "发送结束。")
            message += "请在路由器客户端列表确认 A1 是否获得 IP；发送完成不代表配网成功。"
            with self.lock:
                self.state.update(phase=phase, count=count, cycles=cycles, message=message)
        except Exception:
            # Socket errors must not expose multicast destinations carrying credentials.
            with self.lock:
                self.state.update(phase="error", message="发送失败：请检查无线网卡 IPv4、组播路由和防火墙。")
        finally:
            with self.lock:
                self.state["finished_elapsed"] = round(time.monotonic() - self.started, 1)

    def stop(self):
        self.cancel.set()

    def close(self):
        self.stop()
        if self.worker is not None:
            self.worker.join(timeout=2)


class LocalServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port=8765, provisioning=None):
        super().__init__(("127.0.0.1", port), Handler)
        self.origin = f"http://127.0.0.1:{self.server_port}"
        self.token = secrets.token_urlsafe(32)
        self.provisioning = provisioning if provisioning is not None else Provisioning()

    def server_close(self):
        self.provisioning.close()
        super().server_close()


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, format, *args):
        pass  # No request URLs, bodies, credentials or multicast destinations in logs.

    def respond(self, status, body, content_type="application/json; charset=utf-8"):
        if isinstance(body, dict):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; "
                         "script-src 'self'; style-src 'self'; frame-ancestors 'none'; "
                         "base-uri 'none'; form-action 'none'")
        self.end_headers()
        self.wfile.write(body)

    def valid_host(self):
        return self.headers.get("Host") == self.server.origin.removeprefix("http://")

    def do_GET(self):
        if not self.valid_host():
            return self.respond(403, {"error": "仅允许本机页面访问"})
        if self.path == "/api/status":
            return self.respond(200, self.server.provisioning.snapshot())
        assets = {"/": ("index.html", "text/html; charset=utf-8"),
                  "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                  "/style.css": ("style.css", "text/css; charset=utf-8")}
        if self.path not in assets:
            return self.respond(404, {"error": "页面不存在"})
        name, content_type = assets[self.path]
        body = (ASSETS / name).read_bytes()
        if name == "index.html":
            body = body.replace(b"{{TOKEN}}", self.server.token.encode("ascii"))
        return self.respond(200, body, content_type)

    def do_POST(self):
        supplied_token = self.headers.get("X-A1-Token", "")
        if (not self.valid_host() or self.headers.get("Origin") != self.server.origin
                or not supplied_token.isascii()
                or not secrets.compare_digest(supplied_token, self.server.token)):
            return self.respond(403, {"error": "请从工具打开的本机页面操作"})
        if self.path not in ("/api/start", "/api/stop"):
            return self.respond(404, {"error": "接口不存在"})
        if self.headers.get_content_type() != "application/json":
            return self.respond(415, {"error": "需要 application/json"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4096 or self.headers.get("Transfer-Encoding"):
                raise ValueError("请求大小不正确")
            data = json.loads(self.rfile.read(length))
            if self.path == "/api/start":
                self.server.provisioning.start(data)
            else:
                self.server.provisioning.stop()
            return self.respond(200, self.server.provisioning.snapshot())
        except (UnicodeDecodeError, json.JSONDecodeError):
            return self.respond(400, {"error": "JSON 格式不正确"})
        except ValueError as exc:
            return self.respond(400, {"error": str(exc)})
        except RuntimeError as exc:
            return self.respond(409, {"error": str(exc)})


def main():
    parser = argparse.ArgumentParser(description="A1 原厂 EasyLink 网页配网工具（仅本机）")
    parser.add_argument("--port", type=int, default=8765, help="本机页面端口，默认 8765")
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("页面端口须在 0–65535 之间；0 表示自动选择")
    try:
        with LocalServer(args.port) as server:
            print(f"A1 本机配网页面：{server.origin}", flush=True)
            print("电脑须连接目标 2.4 GHz Wi-Fi。按 Ctrl+C 退出并停止发送。", flush=True)
            if not args.no_browser:
                webbrowser.open(server.origin)
            try:
                server.serve_forever(poll_interval=0.2)
            except KeyboardInterrupt:
                print("\n正在退出并停止发送。")
    except OSError:
        parser.exit(1, "无法启动本机服务；端口可能已被占用，可加 --port 0 自动选择。\n")


if __name__ == "__main__":
    main()
