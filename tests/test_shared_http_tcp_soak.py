"""Real loopback TCP soak against the production shared HTTP core/parser."""
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import random
import re
import select
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = Path(os.environ.get("PH_HTTP_SOURCE_ROOT", ROOT)).resolve()
SECONDS = os.environ.get("PH_HTTP_TCP_SOAK_SECONDS", "2")
CYCLES = os.environ.get("PH_HTTP_TCP_SOAK_CYCLES", "8")
SEED = os.environ.get("PH_HTTP_TCP_SOAK_SEED", "0x7c391")
MAX_RESPONSE = 64 * 1024
CLIENT_DEADLINE = 2.5


def bounded_int(name, raw, low, high, base=10):
    try:
        value = int(raw, base)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if not low <= value <= high:
        raise ValueError(f"{name} must be between {low} and {high}")
    return value


def _set_remaining_timeout(sock, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("client exchange deadline expired")
    sock.settimeout(remaining)


def receive_response(sock, deadline):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        _set_remaining_timeout(sock, deadline)
        part = sock.recv(4096)
        if not part:
            return None if not data else (None, bytes(data), 0, False, False)
        data.extend(part)
        if len(data) > 8192:
            return (None, bytes(data), 0, False, False)
    head, body = bytes(data).split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    match = re.fullmatch(rb"HTTP/1\.[01] ([0-9]{3}) [\t\x20-\x7e\x80-\xff]*",
                         lines[0])
    if not match:
        return (None, bytes(data), 0, False, False)
    status = int(match.group(1))
    headers = {}
    for line in lines[1:]:
        if b":" not in line:
            return (status, bytes(body), 0, False, False)
        name, value = line.split(b":", 1)
        if not re.fullmatch(rb"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name):
            return (status, bytes(body), 0, False, False)
        if any((byte < 32 and byte != 9) or byte == 127 for byte in value):
            return (status, bytes(body), 0, False, False)
        headers.setdefault(name.lower(), []).append(value.strip())
    if headers.get(b"connection") != [b"close"]:
        return (status, bytes(body), 0, False, False)
    length = 0
    if status != 304:
        values = headers.get(b"content-length", [])
        if len(values) != 1 or not values[0].isdigit():
            return (status, bytes(body), 0, False, False)
        length = int(values[0])
    if length > MAX_RESPONSE:
        return (status, bytes(body), length, False, False)
    body = bytearray(body)
    if len(body) > length:
        return (status, bytes(body), length, False, False)
    while len(body) < length:
        _set_remaining_timeout(sock, deadline)
        part = sock.recv(min(4096, length - len(body)))
        if not part:
            return (status, bytes(body), length, True, False)
        body.extend(part)
    return (status, bytes(body), length, True, True)


def classify_response(response, expected_body):
    if response is None:
        return "transport"
    status, body, length, valid_headers, complete = response
    if status is None or not valid_headers:
        return "other_http"
    if not complete:
        return "transport"
    if status == 503:
        return "http_503" if body == b'{"code":"busy"}' else "other_http"
    if status == 200 and length == len(expected_body) and body == expected_body:
        return "complete"
    return "other_http"


def exchange(port, wire, expected_body=b"ok"):
    deadline = time.monotonic() + CLIENT_DEADLINE
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        _set_remaining_timeout(sock, deadline)
        sock.connect(("127.0.0.1", port))
        _set_remaining_timeout(sock, deadline)
        sock.sendall(wire)
        return classify_response(receive_response(sock, deadline), expected_body)
    except (ConnectionError, OSError, TimeoutError):
        return "transport"
    finally:
        sock.close()


def intentional_abort(port, wire, *, reset=False, shutdown_write=False, settle=0.0):
    deadline = time.monotonic() + CLIENT_DEADLINE
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        _set_remaining_timeout(sock, deadline)
        sock.connect(("127.0.0.1", port))
        if reset:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
        _set_remaining_timeout(sock, deadline)
        sock.sendall(wire)
        if shutdown_write:
            sock.shutdown(socket.SHUT_WR)
        if settle:
            time.sleep(min(settle, max(0.0, deadline - time.monotonic())))
        return True
    except (ConnectionError, OSError, TimeoutError):
        return False
    finally:
        sock.close()


def fd_count(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


class SharedHTTPRealTCPSOAK(unittest.TestCase):
    def test_real_tcp_mixed_soak_caps_three_and_four(self):
        seconds = bounded_int("PH_HTTP_TCP_SOAK_SECONDS", SECONDS, 1, 14400)
        cycles = bounded_int("PH_HTTP_TCP_SOAK_CYCLES", CYCLES, 1, 100000)
        seed = bounded_int("PH_HTTP_TCP_SOAK_SEED", SEED, 0, 0xFFFFFFFF, 0)
        httpd = SOURCE_ROOT / "common/httpd"
        self.assertTrue((httpd / "http.h").is_file(), httpd)

        with tempfile.TemporaryDirectory() as temp:
            for cap in (3, 4):
                exe = Path(temp) / f"shared-http-tcp-soak-{cap}"
                subprocess.run(
                    [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                     "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
                     "-fno-omit-frame-pointer", "-no-pie",
                     f"-DPH_HTTP_CLIENT_SLOTS={cap}", "-I" + str(httpd),
                     str(ROOT / "tests/http_core/loopback.c"),
                     str(httpd / "core.c"), str(httpd / "parser.c"),
                     "-o", str(exe)], check=True, timeout=30,
                )
                self._run_cap(exe, cap, seconds, cycles, seed)

    def _run_cap(self, exe, cap, seconds, cycles, seed):
        proc = subprocess.Popen(
            [str(exe), "--cap", str(cap)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        try:
            ready, _, _ = select.select([proc.stdout], [], [], 5.0)
            self.assertTrue(ready, "server startup exceeded 5-second bound")
            line = proc.stdout.readline().strip()
            self.assertTrue(line.isdigit(), f"server did not publish loopback port: {line!r}")
            port = int(line)
            baseline_fds = fd_count(proc.pid)
            counts = Counter({"total.complete": 0, "total.http_503": 0,
                              "total.transport": 0, "total.other_http": 0})
            started = time.monotonic()
            hard_deadline = started + seconds + 20.0
            rng = random.Random(seed ^ cap)
            cycle = 0
            small = b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n"
            large = b"GET /large HTTP/1.1\r\nConnection: close\r\n\r\n"
            partial = b"GET / HTTP/1.1\r\nX-Partial:"
            truncated = b"POST /body HTTP/1.1\r\nContent-Length: 4096\r\n\r\nabc"

            while cycle < cycles or time.monotonic() - started < seconds:
                self.assertLess(time.monotonic(), hard_deadline,
                                f"cap {cap} soak exceeded bounded duration")
                outcomes = [
                    ("small", exchange(port, small)),
                    ("large", exchange(port, large, b"a" * 32768)),
                ]
                variants = ["partial_header", "rst", "truncated_body"]
                rng.shuffle(variants)
                for variant in variants:
                    if variant == "partial_header":
                        sent = intentional_abort(port, partial, settle=0.01)
                        counts[f"intentional_abort.{variant}"] += 1
                        if not sent:
                            counts[f"intentional_abort_setup_error.{variant}"] += 1
                    elif variant == "rst":
                        sent = intentional_abort(port, small, reset=True)
                        counts["intentional_abort.rst"] += 1
                        if not sent:
                            counts["intentional_abort_setup_error.rst"] += 1
                    else:
                        sent = intentional_abort(port, truncated, shutdown_write=True)
                        counts["intentional_abort.truncated_body"] += 1
                        if not sent:
                            counts["intentional_abort_setup_error.truncated_body"] += 1

                with ThreadPoolExecutor(max_workers=16) as pool:
                    burst = list(pool.map(lambda _: exchange(port, small), range(16)))
                outcomes.extend(("burst", outcome) for outcome in burst)
                for scenario, outcome in outcomes:
                    counts[f"{scenario}.{outcome}"] += 1
                    counts[f"total.{outcome}"] += 1
                cycle += 1

            recovery = [exchange(port, small) for _ in range(3)]
            self.assertEqual(recovery, ["complete"] * 3,
                             f"cap {cap} healthy recovery outcomes: {recovery}")
            counts["recovery.complete"] = len(recovery)

            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and fd_count(proc.pid) != baseline_fds:
                time.sleep(0.02)
            after_clients_fds = fd_count(proc.pid)
            self.assertEqual(after_clients_fds, baseline_fds,
                             f"cap {cap} server fd count did not return to baseline")
            elapsed = time.monotonic() - started

            proc.stdin.write("q")
            proc.stdin.flush()
            stdout, stderr = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 0, stderr)
            self.assertNotIn("Sanitizer", stderr)
            report = re.search(r"^TCP_SOAK (.+)$", stderr, re.MULTILINE)
            self.assertIsNotNone(report, stderr)
            fields = dict(re.findall(r"([a-z_]+)=(-?[0-9]+)", report.group(1)))
            fields = {key: int(value) for key, value in fields.items()}
            self.assertEqual(fields["cap"], cap)
            self.assertEqual(fields["accepted"], fields["closed"], stderr)
            self.assertEqual(fields["begun"], fields["released"], stderr)
            self.assertEqual(fields["rejected"],
                             fields["busy_sent"] + fields["busy_failed"], stderr)
            self.assertEqual(fields["active"], -1, stderr)
            self.assertEqual(fields["queued"], -1, stderr)
            self.assertEqual(fields["free_slots"], cap, stderr)
            self.assertEqual(fields["close_failures"], 0, stderr)
            self.assertGreater(fields["accepted"], 0, stderr)
            self.assertGreater(counts["total.complete"], 0)
            self.assertEqual(counts["total.other_http"], 0,
                             f"cap {cap} unexpected/malformed HTTP responses: {counts}")
            print(
                f"TCP loopback soak PASS cap={cap} seconds={seconds} "
                f"elapsed={elapsed:.2f} cycles={cycle} seed=0x{seed:08x} "
                f"outcomes={dict(sorted(counts.items()))} core={fields} "
                f"server_fds={baseline_fds}->{after_clients_fds}; "
                "503 counts are observational (deterministic overload is covered by core soak)"
            )
        finally:
            if proc.poll() is None:
                try:
                    proc.stdin.write("q")
                    proc.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
