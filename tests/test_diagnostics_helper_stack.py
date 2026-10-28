"""Compact diagnostic helper calls remain visible to the actual stack gate."""
from contextlib import redirect_stdout
import io
import unittest

from tools.status_stack import diagnostic_paths
from tools.check_http_core import validate_paths

HELPERS = ('diagnostic_i32','diagnostic_u32','diagnostic_bool','diagnostic_text')


class DiagnosticsHelperStack(unittest.TestCase):
    def fixture(self,model='A1'):
        paths=diagnostic_paths({},model,'diagnostic',[],3072)
        frames={n:16 for names,reserve in paths.values() for n in names}
        frames.update({n:16 for n in HELPERS+('diagnostic_field',)})
        return frames,diagnostic_paths(frames,model,'diagnostic',[],3072,frames)

    def test_each_emitted_helper_is_a_charged_sibling(self):
        for model in ('A1','M1','TC1'):
            frames,paths=self.fixture(model)
            for helper in HELPERS:
                calls=[names for label,(names,reserve) in paths.items() if helper in label]
                self.assertTrue(calls)
                for names in calls:
                    self.assertIn(helper,names)
                    self.assertEqual(sum(n in HELPERS for n in names),1)
                    self.assertIn('device_diagnostics_mp',names)
                    self.assertIn('service_product_diagnostics_mp',names)
            with redirect_stdout(io.StringIO()):
                validate_paths({k:(r,n) for k,(n,r) in paths.items()},frames)

    def test_emitted_helper_missing_su_and_actual_overflow_fail(self):
        for helper in HELPERS:
            frames,paths=self.fixture();symbols=dict(frames);del frames[helper]
            with self.assertRaisesRegex(ValueError,'Emitted helper missing'):
                diagnostic_paths(frames,'A1','diagnostic',[],3072,symbols)
            frames,paths=self.fixture();frames[helper]=10000
            with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):
                validate_paths({k:(r,n) for k,(n,r) in paths.items()},frames)

    def test_eliminated_product_helpers_are_not_required(self):
        frames,paths=self.fixture('TC1')
        for helper in ('diagnostic_bool','diagnostic_text'):del frames[helper]
        paths=diagnostic_paths(frames,'TC1','diagnostic',[],3072,frames)
        names={n for chain,reserve in paths.values() for n in chain}
        self.assertNotIn('diagnostic_bool',names)
        self.assertNotIn('diagnostic_text',names)


if __name__=='__main__':unittest.main()
