"""Exercise CI's MQTT overlay handoff against the actual pinned SDK objects."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.apply_sdk_patch import changed_paths

ROOT = Path(__file__).resolve().parents[1]
PATCHES = [ROOT / name for name in (
    'common/mqtt-mico36.patch', 'common/mqtt-publish-bounds.patch',
    'common/mqtt-socket-close.patch', 'M1/mqtt-packet-deadline.patch',
    'common/mqtt-response-deadline.patch', 'common/mqtt-buffer-storage.patch')]


class MQTTOverlayPreparationTests(unittest.TestCase):
    def test_fresh_stack_is_idempotent_and_partial_preparation_is_rejected(self):
        # Read only the few touched upstream files, never copy or mutate a
        # product SDK. CI has this bare repository from its pinned SDK cache.
        repository = Path(os.environ.get('PHICOMM_TEST_MICO_GIT_DIR', ROOT / 'build/mico-upstream.git'))
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


if __name__ == '__main__':
    unittest.main()
