import subprocess
import tempfile
import unittest
from pathlib import Path
from tools.apply_sdk_patch import changed_paths

ROOT = Path(__file__).resolve().parents[1]
APPLY = ROOT / 'tools' / 'apply_sdk_patch.py'


class ApplySdkPatchTests(unittest.TestCase):
    def test_ordered_stack_preserves_edits_and_rejects_partial_or_incompatible_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            sdk = root / 'sdk'
            sdk.mkdir()
            target = sdk / 'target.c'
            first, second = root / 'first.patch', root / 'second.patch'
            context = 'int a = 0;\nint b = 0;\nint c = 0;\n'
            for patch, before, after in [(first, 1, 2), (second, 2, 3)]:
                patch.write_text('--- a/target.c\n+++ b/target.c\n@@ -1,4 +1,4 @@\n'
                                 f'-int value = {before};\n+int value = {after};\n' +
                                 ''.join(' ' + line for line in context.splitlines(keepends=True)))
            for prefix in [1, 2, 3]:
                target.write_text(f'int value = {prefix};\n' + context + '/* keep local edit */\n')
                self.apply_patch(sdk, first, second)
                self.assertEqual(target.read_text(), 'int value = 3;\n' + context + '/* keep local edit */\n')
                self.apply_patch(sdk, first, second)
                self.assertEqual(target.read_text(), 'int value = 3;\n' + context + '/* keep local edit */\n')
            target.write_text('int value = 77;\n')
            result = subprocess.run(['python3', str(APPLY), str(sdk), str(first), str(second)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SDK left unchanged', result.stderr)
            self.assertEqual(target.read_text(), 'int value = 77;\n')

    def test_real_json_stack_reruns_from_every_prefix_for_available_products(self):
        patches = [ROOT / 'common' / name for name in (
            'json-newlib-reent.patch', 'json-printbuf-oom.patch', 'json-allocation-safety.patch',
            'json-heap-budget.patch', 'json-newlib-nano-abi.patch', 'json-integer-parse.patch')]
        paths = set().union(*(changed_paths(p.read_bytes()) for p in patches))
        candidates = [ROOT / 'build' / name / 'mico-os' for name in
                      ('a1-sdk', 'm1-sdk', 'tc1-sdk-3.6.4')]
        available = [sdk for sdk in candidates if (sdk / '.git').exists()]
        if not available:
            self.skipTest('pinned product SDK checkout unavailable')
        for sdk_source in available:
            product = sdk_source.parent.name
            baseline = {}
            for path in paths:
                result = subprocess.run(['git', '-C', str(sdk_source), 'show', f'HEAD:{path}'],
                                        capture_output=True)
                if result.returncode == 0:
                    baseline[path] = result.stdout
                else:
                    self.assertIn(path, {'libraries/utilities/json_c/' + name for name in
                                         ('json_number_reent.h', 'json_heap.c', 'json_heap.h')})
            for prefix in range(len(patches) + 1):
                with self.subTest(product=product, prefix=prefix), tempfile.TemporaryDirectory() as temporary:
                    sdk = Path(temporary) / 'sdk'
                    for path, data in baseline.items():
                        target = sdk / path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(data)
                    note = sdk / 'unrelated-local.txt'
                    note.write_text('keep this dirty SDK file\n')
                    source = sdk / 'libraries/utilities/json_c/json_object.c'
                    source.write_bytes(source.read_bytes() + b'\n/* keep unrelated source edit */\n')
                    for patch in patches[:prefix]:
                        subprocess.run(['git', 'apply', '--ignore-whitespace', str(patch)],
                                       cwd=sdk, check=True)
                    self.apply_patch(sdk, *patches)
                    final = {p: (sdk / p).read_bytes() for p in paths}
                    self.apply_patch(sdk, *patches)
                    self.assertEqual(final, {p: (sdk / p).read_bytes() for p in paths})
                    self.assertIn(b'keep unrelated source edit', source.read_bytes())
                    self.assertEqual(note.read_text(), 'keep this dirty SDK file\n')

                    # Missing additions and modified implementations are not a
                    # known prefix. Reject both without changing any SDK file.
                    header = sdk / 'libraries/utilities/json_c/json_number_reent.h'
                    for invalid in [None, b'incompatible local header\n']:
                        with self.subTest(invalid='missing' if invalid is None else 'modified'):
                            if invalid is None:
                                header.unlink()
                            else:
                                header.write_bytes(invalid)
                            before = {p: (sdk / p).read_bytes() if (sdk / p).exists() else None
                                      for p in paths}
                            result = subprocess.run(['python3', str(APPLY), str(sdk), *map(str, patches)],
                                                    capture_output=True, text=True)
                            self.assertNotEqual(result.returncode, 0)
                            self.assertIn('SDK left unchanged', result.stderr)
                            self.assertEqual(before, {
                                p: (sdk / p).read_bytes() if (sdk / p).exists() else None for p in paths})

    def test_nested_archive_is_patched_and_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(outer)], check=True)
            sdk = outer / 'sdk' / 'mico-os'
            target = sdk / 'libraries' / 'target.c'
            target.parent.mkdir(parents=True)
            target.write_text('int value = 1;\n')
            patch = outer / 'overlay.patch'
            patch.write_text(
                'diff --git a/libraries/target.c b/libraries/target.c\n'
                '--- a/libraries/target.c\n'
                '+++ b/libraries/target.c\n'
                '@@ -1 +1 @@\n'
                '-int value = 1;\n'
                '+int value = 2;\n')

            self.apply_patch(sdk, patch)
            self.assertEqual(target.read_text(), 'int value = 2;\n')
            self.apply_patch(sdk, patch)
            self.assertEqual(target.read_text(), 'int value = 2;\n')

            target.write_text('int value = 3;\n')
            result = subprocess.run(['python3', str(APPLY), str(sdk), str(patch)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), 'int value = 3;\n')

    def test_sdk_git_repository_is_patched_and_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / 'sdk'
            sdk.mkdir()
            subprocess.run(['git', 'init', '-q', str(sdk)], check=True)
            target = sdk / 'target.c'
            target.write_text('int value = 1;\n')
            patch = root / 'overlay.patch'
            patch.write_text(
                'diff --git a/target.c b/target.c\n'
                '--- a/target.c\n'
                '+++ b/target.c\n'
                '@@ -1 +1 @@\n'
                '-int value = 1;\n'
                '+int value = 2;\n')

            self.apply_patch(sdk, patch)
            self.assertEqual(target.read_text(), 'int value = 2;\n')
            self.apply_patch(sdk, patch)
            self.assertEqual(target.read_text(), 'int value = 2;\n')

    def test_plain_sdk_preserves_lf_when_patch_context_is_crlf(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(outer)], check=True)
            sdk = outer / 'sdk'
            sdk.mkdir()
            target = sdk / 'target.c'
            target.write_bytes(b'int value = 1;\n')
            patch = outer / 'overlay.patch'
            patch.write_bytes(
                b'diff --git a/target.c b/target.c\n'
                b'--- a/target.c\n'
                b'+++ b/target.c\n'
                b'@@ -1 +1 @@\n'
                b'-int value = 1;\r\n'
                b'+int value = 2;\r\n')

            self.apply_patch(sdk, patch)
            self.assertEqual(target.read_bytes(), b'int value = 2;\n')

    @staticmethod
    def apply_patch(sdk, *patches):
        subprocess.run(['python3', str(APPLY), str(sdk), *map(str, patches)], check=True)


if __name__ == '__main__':
    unittest.main()
