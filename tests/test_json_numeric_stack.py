"""Require actual bounded numeric frames and retain real nested allocator tails."""
from pathlib import Path
import tempfile
import unittest

from tools.json_numeric_stack import (CORE_FILES, JSON_SYMBOLS, NUMERIC_SYMBOLS,
                                      numeric_tails, read_numeric_frames)
from tests import test_mqtt_stack_budget as mqtt_budget


class NumericStack(unittest.TestCase):
    def fixture(self, root):
        directory = root/'utilities/json_c'
        directory.mkdir(parents=True)
        (directory/'json_heap.su').write_text('heap.c:1:1:json_heap_reject\t8\tstatic\n')
        for stem in (*CORE_FILES, 'json_object', 'json_tokener'):
            (directory/(stem+'.su')).write_text('')
        names = set(NUMERIC_SYMBOLS + list(JSON_SYMBOLS)) - {'json_heap_reject'}
        (directory/'context.su').write_text(''.join(
            'source.c:1:1:'+name+'\t8\tstatic\n' for name in sorted(names)))
        return directory

    def test_missing_or_unbounded_actual_frames_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.fixture(root)
            frames = {'json_heap_reject': 8}
            read_numeric_frames(root, frames)
            self.assertTrue(set(NUMERIC_SYMBOLS) <= frames.keys())
            original = (directory/'context.su').read_text()
            (directory/'context.su').write_text(original.replace('json_num_calloc\t8\tstatic', 'json_num_calloc\t8\tdynamic'))
            with self.assertRaisesRegex(ValueError, 'Unbounded numeric frame'):
                read_numeric_frames(root, {'json_heap_reject': 8})
            (directory/'context.su').write_text('\n'.join(
                line for line in original.splitlines() if ':json_num_calloc\t' not in line)+'\n')
            with self.assertRaisesRegex(ValueError, 'Missing actual numeric'):
                read_numeric_frames(root, {'json_heap_reject': 8})
            (directory/'scan.su').unlink()
            with self.assertRaisesRegex(ValueError, 'actual SDK numeric scan'):
                read_numeric_frames(root, {'json_heap_reject': 8})

    def test_policy_and_cleanup_locks_are_independent_nested_tails(self):
        tails = numeric_tails({}, 'scan')
        policy = tails['policy0 json_heap_lock']
        self.assertEqual(policy[-3:], ['json_num_resource_error', 'json_heap_reject', 'json_heap_lock'])
        self.assertIn('json_num_scan_token', policy)
        self.assertNotIn('json_heap_calloc', policy)
        self.assertEqual(tails['cleanup json_heap_unlock'],
                         ['json_number_scan', 'json_num_cleanup', 'json_heap_free', 'json_heap_unlock'])
        self.assertNotIn('json_num_scan_token', tails['cleanup json_heap_unlock'])

    def test_real_numeric_nesting_overflow_rejected(self):
        gate = mqtt_budget.MqttStackBudget()
        # Each alone fits. Together with retained converter callers they do not.
        with self.assertRaisesRegex(ValueError, '>16384'):
            gate.run_budget({'json_num_scan_token': 7000, 'json_num_strtod_r': 6000})
        with self.assertRaisesRegex(ValueError, '>16384'):
            gate.run_budget({'json_number_format': 7000, 'json_num_dtoa_r': 6000})
        for missing in ('json_num_cleanup', 'json_num_resource_error', 'json_heap_reject'):
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, 'missing actual .su'):
                gate.run_budget(missing=missing)


if __name__ == '__main__':
    unittest.main()
