"""Exercise the backup client against a real local MessagePack HTTP endpoint."""
import base64
import contextlib
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

import msgpack

spec = importlib.util.spec_from_file_location("backup", Path(__file__).resolve().parents[1] / "tools/backup_screen_boot.py")
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)
BOOT = bytes((i * 137 ^ (i >> 8)) & 255 for i in range(0x14000))


class BackupTest(unittest.TestCase):
    def run_client(self, fault=""):
        calls, pending = [], {}
        session, reads = 1234, 0

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                nonlocal session, reads
                self.assert_path = self.path == "/api/v1/command"
                envelope = msgpack.unpackb(self.rfile.read(int(self.headers["Content-Length"])), raw=False)
                action, params = envelope["action"], envelope["params"]
                calls.append(action)
                if ((fault == "query_disconnect" and action == "screen.boot.result.get"
                         and calls.count(action) == 1)
                        or (fault == "read_disconnect" and action == "screen.boot.read")):
                    self.close_connection = True
                    return
                data, code = {}, "ok"
                if action == "status.get":
                    data = {"hardware_model": "BRY88BBA15G"}
                elif action == "screen.recovery.get":
                    data = {"available": True, "session": session}
                elif action == "screen.boot.read":
                    offset, n = params["offset"], params["length"]
                    assert 0 <= offset < len(BOOT) and n == 256
                    reads += 1
                    raw = BOOT[offset:offset + n]
                    if fault == "different" and reads > len(BOOT) // 256:
                        raw = bytes([raw[0] ^ 1]) + raw[1:]
                    pending[reads] = {"available": True, "session": session,
                        "state": "complete", "result": 0, "offset": offset,
                        "total": len(BOOT), "length": n,
                        "bytes": base64.b64encode(raw).decode(),
                        "crc32": zlib.crc32(raw) ^ (fault == "crc")}
                    if fault == "session":
                        pending[reads]["session"] += 1
                    code, data = "accepted", {"request": reads, "session": session}
                elif action == "screen.boot.result.get":
                    data = pending[params["request"]]
                else:
                    raise AssertionError(f"Unexpected write/reset: {action}")
                raw = msgpack.packb({"code": code, "data": data}, use_bin_type=True)
                self.send_response(200)
                self.send_header("Content-Type", "application/msgpack")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        with tempfile.TemporaryDirectory() as tmp:
            server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                args = ["backup", "--url", f"http://127.0.0.1:{server.server_port}", "--output", tmp]
                with patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
                    if fault not in ("", "query_disconnect"):
                        with self.assertRaises((ValueError, RuntimeError, http.client.HTTPException)):
                            backup.main()
                        self.assertFalse((Path(tmp) / "boot-backup.json").exists())
                        self.assertTrue(list(Path(tmp).glob("*.partial")) or fault == "different")
                    else:
                        backup.main()
                        report = json.loads((Path(tmp) / "boot-backup.json").read_text())
                        self.assertTrue(report["matching"])
                        self.assertEqual(len(report["reads"]), 2)
                        self.assertEqual(report["reads"][0]["sha256"], hashlib.sha256(BOOT).hexdigest())
                        self.assertEqual((Path(tmp) / "boot-read1.bin").read_bytes(), BOOT)
                        self.assertEqual((Path(tmp) / "boot-read2.bin").read_bytes(), BOOT)
                        with self.assertRaises(FileExistsError):
                            backup.main()
                self.assertTrue(all(a in {"status.get", "screen.recovery.get", "screen.boot.read", "screen.boot.result.get"} for a in calls))
                if fault == "read_disconnect":
                    self.assertEqual(calls.count("screen.boot.read"), 1)
                if fault == "query_disconnect":
                    self.assertEqual(calls.count("screen.boot.read"), 2 * len(BOOT) // 256)
                    self.assertEqual(calls.count("screen.boot.result.get"), 2 * len(BOOT) // 256 + 1)
            finally:
                server.shutdown()
                server.server_close()
                worker.join()

    def test_matching_full_reads(self): self.run_client()
    def test_crc_rejected(self): self.run_client("crc")
    def test_reset_session_rejected(self): self.run_client("session")
    def test_different_full_reads_rejected(self): self.run_client("different")
    def test_query_disconnect_retried(self): self.run_client("query_disconnect")
    def test_read_disconnect_never_repeated(self): self.run_client("read_disconnect")


if __name__ == "__main__": unittest.main()
