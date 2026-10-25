"""Production A1 topic generators and broker wire/shared-memory boundaries."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class HubTopicBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='hub-topic-budget-')
        cls.addClassCleanup(cls.tmp.cleanup)
        directory = Path(cls.tmp.name)
        runtime = (ROOT / 'common/runtime.c').read_text()
        a1 = (ROOT / 'A1/mqtt.c').read_text()
        # Compile the actual caller capacities as well as the real generators;
        # a smaller receiver must fail before a complete topic is published.
        discovery = re.search(r'char owner\[13\], device_id\[([^]]+)\]', runtime).group(1)
        parent = re.search(r'static char parent_id\[([^]]+)\]', a1).group(1)
        (directory / 'generated_topics.h').write_text(
            '#define FEATURE_HOST\n#include "common/phicomm_features.h"\n#define FEATURE_DEVICE_A1\n'
            '#define LOCAL_ID "aabbccddeeff"\nstatic mqtt_account_t account;\n'
            'static mqtt_account_t mqtt_snapshot() { return account; }\n'
            + function(runtime, 'static void root_for(') + '\n'
            + function(runtime, 'static bool virtual_identity(') + '\n'
            + f'static constexpr size_t DiscoveryIdCapacity = {discovery};\n'
            + f'static constexpr size_t ParentIdCapacity = {parent};\n')
        cls.binary = directory / 'topic-budget'
        subprocess.run(['c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT), '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(directory),
                        str(ROOT / 'DAIKIN/tests/hub/topic_budget.cpp'), '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_actual_long_a1_discovery_and_proxy_tombstone(self):
        self.scenario('generated')

    def test_utf8_topic_and_publish_packet_boundaries(self):
        self.scenario('boundaries')

    def test_long_filters_keep_the_shared_subscription_budget(self):
        self.scenario('subscriptions')

    def test_long_retained_topics_keep_the_shared_retained_budget(self):
        self.scenario('retained')

    def test_long_wills_and_partial_packets_keep_shared_budgets(self):
        self.scenario('will-receive')


if __name__ == '__main__':
    unittest.main()
