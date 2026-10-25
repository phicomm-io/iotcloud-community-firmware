"""Real TCP/WS transport with the exact production byte-heap admission hooks."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class HubAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hub-admission-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        source = (ROOT / 'src/hub.inc').read_text()
        helpers = 'static size_t hubByteHeap() {' + source.split(
            'static size_t hubByteHeap() {', 1)[1].split('static bool hubReload', 1)[0]
        assignments = 'transport.admission = ' + source.split(
            'hubTransport.admission = ', 1)[1].split(
            '  hubTransport.broker.authenticate = ', 1)[0].replace('hubTransport.', 'transport.')
        (build / 'allocation-ready.inc').write_text(helpers)
        (build / 'admission-assignments.inc').write_text(assignments)
        cls.binary = str(build / 'admission')
        subprocess.run([
            'c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
            '-I' + str(ROOT / 'include'), '-I' + str(build),
            str(ROOT / 'tests/hub/admission.cpp'), '-lcrypto', '-Wl,--wrap=send',
            '-o', cls.binary], check=True)

    def scenario(self, name):
        tcp, ws = port(), port()
        while tcp == ws:
            ws = port()
        subprocess.run([self.binary, name, str(tcp), str(ws)], check=True, timeout=15,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_tcp_small_connect_with_actual_reported_fragmentation(self):
        self.scenario('tcp-small')

    def test_ws_small_connect_with_actual_reported_fragmentation(self):
        self.scenario('ws-small')

    def test_tcp_uses_only_initial_and_actual_packet_blocks(self):
        self.scenario('tcp-fragmented')

    def test_ws_uses_only_initial_and_actual_handshake_blocks(self):
        self.scenario('ws-fragmented')

    def test_exact_initial_reserve_block_and_overflow_boundaries(self):
        self.scenario('boundaries')

    def test_low_heap_rejects_new_socket_preserves_existing_ping(self):
        self.scenario('existing')

    def test_tcp_growth_still_rejects_before_oversized_allocation(self):
        self.scenario('tcp-growth')

    def test_ws_mqtt_growth_still_rejects_before_oversized_allocation(self):
        self.scenario('ws-growth')

    def test_ws_http_full_replacement_guard_closes_cleanly(self):
        self.scenario('http-growth')

    def test_ws_actual_nothrow_allocation_failure_closes_cleanly(self):
        self.scenario('http-failure')

    def test_ws_long_unknown_fields_have_no_large_substring_allocation(self):
        self.scenario('long-headers')

    def test_ws_http_limit_does_not_double_past_2049_allocation(self):
        self.scenario('http-limit')

    def test_partial_transmit_does_not_allocate_an_unchecked_shrink_copy(self):
        self.scenario('partial-tx')

    def test_eight_mixed_clients_keep_connection_limit_and_existing_traffic(self):
        self.scenario('clients')


if __name__ == '__main__':
    unittest.main()
