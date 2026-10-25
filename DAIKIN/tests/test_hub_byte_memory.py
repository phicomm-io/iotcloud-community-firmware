"""Production byte-buffer ownership and fragmented-heap allocation guards.

The allocator predicate models available byte-addressable heap. Focused cases
also cap actual C++ allocation sizes to catch unchecked container growth.
Real sockets exercise transport callbacks; no device is used.
"""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class HubByteMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hub-byte-memory-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / 'byte-memory')
        source = (ROOT / 'src/hub.inc').read_text()
        start = source.index('static bool hubAllocationReady(')
        end = source.index('\n}', start) + 2
        (Path(cls.temp.name) / 'hub_input_lifetime_guard.h').write_text(source[start:end] + '\n')
        subprocess.run(['c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(Path(cls.temp.name)), '-I' + str(ROOT / 'include'), str(ROOT / 'tests/hub/byte_memory.cpp'),
                        '-o', cls.binary], check=True)

    def scenario(self, name):
        tcp, ws = port(), port()
        while tcp == ws:
            ws = port()
        subprocess.run([self.binary, name, str(tcp), str(ws)], check=True, timeout=15,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_coalesced_packets_and_every_split_preserve_tail(self):
        self.scenario('coalesced')

    def test_synchronous_disconnect_and_reset_own_packet_bytes(self):
        self.scenario('callback-reset')

    def test_final_fragment_checks_full_receive_reservation(self):
        self.scenario('receive-growth')

    def test_direct_publish_checks_full_transmit_reservation(self):
        self.scenario('direct-growth')

    def test_raw_send_checks_full_transmit_reservation(self):
        self.scenario('raw-growth')

    def test_retained_burst_keeps_allocations_below_fragmented_block(self):
        self.scenario('retained-growth')

    def test_small_packets_work_with_2548_byte_contiguous_block(self):
        self.scenario('small-receive')

    def test_parsed_packet_copies_have_separate_admission(self):
        self.scenario('receive-copy')

    def test_publish_releases_input_before_fixed_reserve_guard_and_observer(self):
        self.scenario('publish-qos0')

    def test_qos1_release_preserves_binary_retained_replacement_replay_and_tail(self):
        self.scenario('publish-qos1-retained')

    def test_duplicate_subscriptions_keep_replay_metadata_bounded(self):
        self.scenario('duplicate-subscriptions')


if __name__ == '__main__':
    unittest.main()
