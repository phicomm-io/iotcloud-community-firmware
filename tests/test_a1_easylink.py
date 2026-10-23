#!/usr/bin/env python3
"""Sender CLI, validation and socket lifecycle checks; no RF/network traffic."""

import contextlib
import io
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import a1_easylink as sender


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class SenderTests(unittest.TestCase):
    def test_credentials_use_utf8_byte_limits(self):
        sender.encode("中" * 10, "12345678", "192.168.1.23")
        with self.assertRaises(ValueError):
            sender.encode("中" * 11, "12345678", "192.168.1.23")

    def test_invalid_credentials(self):
        for ssid, key in (("", "12345678"), ("s\0s", "12345678"),
                          ("s", "1234567"), ("s", "p" * 64),
                          ("s", "1234567\0"), ("s", "p" * 65)):
            with self.subTest(lengths=(len(ssid), len(key))), self.assertRaises(ValueError):
                sender.encode(ssid, key, "192.168.1.23")

    def test_hex_psk_and_open_network(self):
        sender.encode("s", "aF01" * 16, "192.168.1.23")
        sender.encode("s", "", "192.168.1.23")

    def test_interface_rejects_non_unicast_addresses(self):
        for address in ("0.0.0.0", "127.0.0.1", "239.1.2.3", "255.255.255.255",
                        "::1", "not-an-ip"):
            with self.subTest(address=address), self.assertRaises(ValueError):
                sender.interface_ipv4(address)

    def test_record_size_checks(self):
        for record in (b"", b"a", bytes(258)):
            with self.assertRaises(ValueError):
                sender.data_packets(record)

    def test_repr_does_not_disclose_multicast_data(self):
        packet = sender.Datagram("239.126.115.101", 25)
        self.assertNotIn(packet.address, repr(packet))

    def test_socket_interface_payloads_deadline_and_cleanup(self):
        clock, sock = Clock(), MagicMock()
        sock.__enter__.return_value = sock
        packets = sender.data_packets(sender.encode("HomeWiFi", "12345678", "192.168.1.23"))
        with patch.object(sender.socket, "socket", return_value=sock), \
                patch.object(sender.time, "monotonic", clock.monotonic), \
                patch.object(sender.time, "sleep", clock.sleep):
            count, cycles = sender.send(packets, "192.168.1.23", duration=1)
        self.assertAlmostEqual(clock.now, 1)
        self.assertGreater(cycles, 0)
        self.assertEqual(count, sock.sendto.call_count)
        sock.bind.assert_called_once_with(("192.168.1.23", 0))
        sock.setsockopt.assert_any_call(sender.socket.IPPROTO_IP,
                                     sender.socket.IP_MULTICAST_IF,
                                     sender.socket.inet_aton("192.168.1.23"))
        sock.setsockopt.assert_any_call(sender.socket.IPPROTO_IP,
                                     sender.socket.IP_MULTICAST_TTL, 1)
        sock.setsockopt.assert_any_call(sender.socket.IPPROTO_IP,
                                     sender.socket.IP_MULTICAST_LOOP, 0)
        allowed = {p.address: p.length for p in packets}
        allowed[sender.GUIDE_ADDRESS] = 20
        for call in sock.sendto.call_args_list:
            payload, (address, port) = call.args
            self.assertEqual(payload, bytes(allowed[address]))
            self.assertEqual(port, 8001)
        sock.__exit__.assert_called_once()

    def test_socket_error_closes_and_propagates(self):
        sock = MagicMock()
        sock.__enter__.return_value = sock
        sock.sendto.side_effect = OSError("test socket failure")
        packets = sender.data_packets(sender.encode("HomeWiFi", "12345678", "192.168.1.23"))
        with patch.object(sender.socket, "socket", return_value=sock), self.assertRaises(OSError):
            sender.send(packets, "192.168.1.23", duration=1)
        sock.__exit__.assert_called_once()

    def test_stop_interrupts_packet_cycle_and_closes_socket(self):
        cancel = threading.Event()
        sock = MagicMock()
        sock.__enter__.return_value = sock
        sock.sendto.side_effect = lambda *args: cancel.set()
        packets = sender.data_packets(sender.encode("s", "", "192.168.1.23"))
        with patch.object(sender.socket, "socket", return_value=sock):
            count, cycles = sender.send(packets, "192.168.1.23", duration=50, cancel=cancel)
        self.assertEqual((count, cycles), (1, 0))
        sock.__exit__.assert_called_once()

    def test_send_parameter_errors_open_no_socket(self):
        packets = sender.data_packets(sender.encode("s", "", "192.168.1.23"))
        for options in ({"duration": 0}, {"duration": float("nan")}, {"duration": 61},
                        {"interval": 0}, {"interval": float("inf")}, {"port": 0}):
            with patch.object(sender.socket, "socket") as sock, self.assertRaises(ValueError):
                sender.send(packets, "192.168.1.23", **options)
            sock.assert_not_called()

    def test_cli_dry_run_opens_no_socket_and_does_not_print_password(self):
        out, err, secret = io.StringIO(), io.StringIO(), "unique-test-password"
        with patch.object(sender.socket, "socket") as sock, \
                patch.object(sys.stdin, "isatty", return_value=True), \
                patch.object(sender.getpass, "getpass", return_value=secret), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            result = sender.main(["--interface-ip", "192.168.1.23", "--ssid", "HomeWiFi", "--dry-run"])
        self.assertEqual(result, 0)
        sock.assert_not_called()
        self.assertNotIn(secret, out.getvalue() + err.getvalue())
        self.assertIn("未发送", out.getvalue())

    def test_cli_does_not_read_password_from_pipe(self):
        with patch.object(sys.stdin, "isatty", return_value=False), \
                patch.object(sender.getpass, "getpass") as password, \
                contextlib.redirect_stderr(io.StringIO()):
            result = sender.main(["--interface-ip", "192.168.1.23", "--ssid", "s"])
        self.assertEqual(result, 2)
        password.assert_not_called()

    def test_cli_open_dry_run(self):
        with patch.object(sender.socket, "socket") as sock, \
                contextlib.redirect_stdout(io.StringIO()):
            result = sender.main(["--interface-ip", "192.168.1.23", "--ssid", "s", "--open", "--dry-run"])
        self.assertEqual(result, 0)
        sock.assert_not_called()

    def test_cli_completion_does_not_claim_device_success(self):
        output = io.StringIO()
        with patch.object(sender, "send", return_value=(100, 1)), \
                contextlib.redirect_stdout(output):
            result = sender.main(["--interface-ip", "192.168.1.23", "--ssid", "s", "--open"])
        self.assertEqual(result, 0)
        self.assertIn("只是发送完成", output.getvalue())

    def test_cli_os_error_and_interrupt(self):
        for exception, status in ((OSError("test network error"), 2), (KeyboardInterrupt(), 130)):
            with patch.object(sender, "send", side_effect=exception), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                result = sender.main(["--interface-ip", "192.168.1.23", "--ssid", "s", "--open"])
            self.assertEqual(result, status)


if __name__ == "__main__":
    unittest.main()
