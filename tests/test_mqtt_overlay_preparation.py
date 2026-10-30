"""Exercise CI's MQTT overlay handoff against the actual pinned SDK objects."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.apply_sdk_patch import changed_paths, apply_stack
from tests import test_shared_mqtt as shared

ROOT = Path(__file__).resolve().parents[1]
PATCHES = [ROOT / name for name in (
    'common/mqtt-mico36.patch', 'common/mqtt-publish-bounds.patch',
    'common/mqtt-socket-close.patch', 'M1/mqtt-packet-deadline.patch',
    'common/mqtt-response-deadline.patch', 'common/mqtt-buffer-storage.patch',
    'common/mqtt-subscribe-qos.patch', 'common/mqtt-subscribe-result.patch',
    'common/mqtt-receive-recovery.patch', 'common/mqtt-post-send-workspace.patch')]


class MQTTOverlayPreparationTests(unittest.TestCase):
    def test_all_product_builders_use_the_complete_ordered_stack(self):
        expected = [str(p.relative_to(ROOT)) for p in PATCHES]
        for name in ('A1/build.sh', 'M1/build.sh', 'TC1/build.sh',
                     'tests/build_host.sh', 'tests/run_tc1.sh', 'tests/simulator/build.sh'):
            with self.subTest(builder=name):
                text = (ROOT / name).read_text()
                commands = text.replace('\\\n', ' ').splitlines()
                stacks = [re.findall(r'(?:common|M1)/mqtt-[a-z0-9-]+\.patch', line)
                          for line in commands if 'mqtt-buffer-storage.patch' in line]
                self.assertEqual(stacks, [expected])

    def test_all_product_sdk_old_prefix_upgrade_matches_fresh_and_repeats(self):
        # Include each board's real Yield/handler patches before the common
        # stack. All source comes from the pinned SDK, never a mocked client.
        for model in ('A1', 'M1', 'TC1'):
            with self.subTest(model=model):
                fixture = shared.SharedMQTT()
                with fixture.protocol(model, with_qos=False) as old:
                    self.assertIn(b'(int *)&qos', (old / 'MQTTClient.c').read_bytes())
                    sdk = old.parents[2]
                    for repeat in range(2):
                        apply_stack(sdk, PATCHES + ([ROOT / 'TC1/mqtt-yield-errors.patch'] if model == 'TC1' else []))
                    upgraded = {p.relative_to(old): p.read_bytes()
                                for p in old.rglob('*') if p.is_file()}
                with fixture.protocol(model) as fresh:
                    self.assertEqual(upgraded, {p.relative_to(fresh): p.read_bytes()
                                               for p in fresh.rglob('*') if p.is_file()})

    def test_fresh_stack_is_idempotent_and_partial_preparation_is_rejected(self):
        # Read only the few touched upstream files, never copy or mutate a
        # product SDK. CI has this bare repository from its pinned SDK cache.
        tools_home = Path(os.environ.get('PHICOMM_TOOLS_HOME',
                          Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'phicomm'))
        repository = Path(os.environ.get('PHICOMM_TEST_MICO_GIT_DIR',
                          os.environ.get('MICO_SDK_CACHE', tools_home / 'mico-upstream.git')))
        commit = json.loads((ROOT / 'common/mico-sdk.json').read_text())['commit']
        paths = set().union(*(changed_paths(p.read_bytes()) for p in PATCHES))
        baseline = {p: subprocess.check_output(
            ['git', '--git-dir', str(repository), 'show', f'{commit}:{p}']) for p in paths}
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)

            def restore():
                for name, data in baseline.items():
                    target = sdk / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)

            def apply(*patches):
                return subprocess.run([sys.executable, str(ROOT / 'tools/apply_sdk_patch.py'),
                                       str(sdk), *map(str, patches)], capture_output=True, text=True)

            restore()
            # Reproduce the previous workflow's selected pre-applied overlays.
            for patch in (PATCHES[0], *PATCHES[2:5]):
                result = apply(patch)
                self.assertEqual(result.returncode, 0, result.stderr)
            partial = {p: (sdk / p).read_bytes() for p in paths}
            result = apply(*PATCHES)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SDK left unchanged', result.stderr)
            self.assertEqual(partial, {p: (sdk / p).read_bytes() for p in paths})

            # Hand the clean SDK to the host builder as the fixed workflow does.
            restore()
            result = apply(*PATCHES)
            self.assertEqual(result.returncode, 0, result.stderr)
            complete = {p: (sdk / p).read_bytes() for p in paths}
            result = apply(*PATCHES)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(complete, {p: (sdk / p).read_bytes() for p in paths})

            # Every complete prior stack upgrades without replacing its SDK:
            # before QoS, distinct SUBACK refusal, RX recovery or TX retention.
            for prefix in (PATCHES[:6], PATCHES[:7], PATCHES[:8], PATCHES[:9]):
                restore()
                result = apply(*prefix)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(b'MQTT_SUBSCRIBE_DENIED' in (sdk / 'libraries/protocols/mqtt/MQTTClient.h').read_bytes(), len(prefix) >= 8)
                for repeat in range(2):
                    result = apply(*PATCHES)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(complete, {p: (sdk / p).read_bytes() for p in paths})


if __name__ == '__main__':
    unittest.main()
