#!/usr/bin/env python3
"""Exercise the local web service with a fake radio sender; no UDP is sent."""

import http.client
import json
from pathlib import Path
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from a1_easylink import data_packets, encode
from a1_easylink_web import LocalServer, Provisioning


class WebTests(unittest.TestCase):
    def setUp(self):
        self.entered = threading.Event()
        self.release = threading.Event()
        self.calls = []

        def radio(packets, address, **kwargs):
            self.calls.append((packets, address, kwargs))
            self.entered.set()
            kwargs["progress"](42, 1, 0.2)
            while not self.release.wait(0.01):
                if kwargs["cancel"].is_set():
                    break
            return 84, 2

        self.task = Provisioning(radio)
        self.server = LocalServer(0, self.task)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.valid = {"ssid": "斐讯A1", "password": "unique-secret-1234",
                      "interface_ip": "192.168.1.23", "open_network": False,
                      "duration": 50, "interval_ms": 10}

    def tearDown(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method="GET", path="/api/status", data=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        base = {"Origin": self.server.origin, "X-A1-Token": self.server.token,
                "Content-Type": "application/json"}
        base.update(headers or {})
        body = json.dumps(data, ensure_ascii=False).encode() if data is not None else None
        try:
            connection.request(method, path, body=body, headers=base)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_assets_are_local_no_store_and_not_directory_listing(self):
        for path in ("/", "/app.js", "/style.css"):
            status, headers, body = self.request(path=path)
            self.assertEqual(status, 200)
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
            if path == "/":
                self.assertIn(self.server.token.encode(), body)
                self.assertNotIn(b"{{TOKEN}}", body)
        self.assertEqual(self.request(path="/../a1_easylink.py")[0], 404)

    def test_cross_origin_bad_host_and_missing_token_cannot_send(self):
        for header in ({"Origin": "https://example.com"}, {"X-A1-Token": ""},
                       {"Host": "evil.example"}):
            self.assertEqual(self.request("POST", "/api/start", self.valid, header)[0], 403)
        self.assertEqual(self.request(headers={"Host": "evil.example"})[0], 403)
        self.assertFalse(self.calls)

    def test_validation_opens_no_radio_and_does_not_echo_password(self):
        for change in ({"ssid": "中" * 11}, {"interface_ip": "127.0.0.1"},
                       {"password": "short"}, {"duration": 61}, {"interval_ms": 0},
                       {"ssid": None}, {"duration": None}, {"open_network": True},
                       {"open_network": "false"}):
            with self.subTest(change=change):
                status, _, body = self.request("POST", "/api/start", dict(self.valid, **change))
                self.assertEqual(status, 400)
                self.assertNotIn(self.valid["password"].encode(), body)
        self.assertFalse(self.calls)

    def test_start_progress_busy_stop_and_restart(self):
        self.assertEqual(self.request("POST", "/api/start", self.valid)[0], 200)
        self.assertTrue(self.entered.wait(1))
        packets, address, options = self.calls[0]
        self.assertEqual(packets, data_packets(encode(self.valid["ssid"],
                         self.valid["password"], self.valid["interface_ip"])))
        self.assertEqual(address, self.valid["interface_ip"])
        self.assertEqual(options["duration"], 50)
        self.assertEqual(options["interval"], 0.01)
        status, _, body = self.request()
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["phase"], "sending")
        self.assertNotIn(self.valid["password"].encode(), body)
        self.assertNotIn(packets[0].address.encode(), body)
        self.assertEqual(self.request("POST", "/api/start", self.valid)[0], 409)
        self.assertEqual(self.request("POST", "/api/stop", {})[0], 200)
        self.task.worker.join(timeout=1)
        self.assertFalse(self.task.worker.is_alive())
        self.assertEqual(self.task.snapshot()["phase"], "stopped")
        self.release.set()
        self.assertEqual(self.request("POST", "/api/start", self.valid)[0], 200)
        self.task.worker.join(timeout=1)
        self.assertEqual(self.task.snapshot()["phase"], "sent")
        self.assertEqual(self.task.snapshot()["count"], 84)
        self.assertIn("不代表配网成功", self.task.snapshot()["message"])

    def test_open_network(self):
        self.release.set()
        self.assertEqual(self.request("POST", "/api/start",
                         dict(self.valid, open_network=True, password=""))[0], 200)
        self.task.worker.join(timeout=1)
        self.assertEqual(self.task.snapshot()["phase"], "sent")

    def test_network_error_is_sanitized(self):
        def fail(*args, **kwargs):
            raise OSError("239.126.123.45 credentials must not appear")
        self.task.sender = fail
        self.assertEqual(self.request("POST", "/api/start", self.valid)[0], 200)
        self.task.worker.join(timeout=1)
        state = self.task.snapshot()
        self.assertEqual(state["phase"], "error")
        self.assertNotIn("239.126", json.dumps(state))

    def test_server_close_stops_worker(self):
        self.task.start(self.valid)
        self.assertTrue(self.entered.wait(1))
        self.task.close()
        self.assertFalse(self.task.worker.is_alive())


if __name__ == "__main__":
    unittest.main()
