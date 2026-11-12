"""Fail-closed fixtures for the actual typed feature/UART worker gate."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from tools.check_http_core import StackFrames
from tools.check_worker_stack import worker_paths,validate_worker_paths,read_worker_frames

class WorkerMessagePackStackGate(unittest.TestCase):
    def fixture(self,model):
        paths=worker_paths({},model,12288)
        frames=StackFrames()
        frames.update({n:16 for budget,reserve,names in paths.values() for n in names})
        for name in ['auto_execute','phv_scan','phv_head','phv_next','install_config',
                     'group_envelope_mp','group_envelope_token_mp','phw_bytes',
                     'lan_client_response','lc_response','lan_client_save',
                     'm1_config_save_language','m1_config_commit']:
            frames[name]=16
        return frames,worker_paths(frames,model,12288,frames)
    def test_direct_control_udp_pairing_and_original_budgets(self):
        for model in ['A1','M1']:
            frames,paths=self.fixture(model)
            with redirect_stdout(io.StringIO()):validate_worker_paths(paths,frames)
            names={n for budget,reserve,chain in paths.values() for n in chain}
            self.assertTrue({'feature_worker','automation_poll','lan_decode_value',
                             'phv_parse','feature_lan_pair','ph_auth_pair','ph_auth_remote_store'}<=names)
            self.assertFalse(any('json' in n for n in names))
            self.assertNotIn('service_command',names)
            self.assertNotIn('service_query_mp',names)
            self.assertNotIn('service_product_status_mp',names)
            self.assertTrue(all(reserve==2048 and budget in [4096,12288] for budget,reserve,names in paths.values()))
        self.assertIn('m1_automation_execute',names)
        self.assertIn('m1_config_commit',names)
        self.assertIn('ph_auth_credential_commit',names)
    def test_missing_core_or_emitted_helper_rejected(self):
        for missing in ['feature_worker','automation_poll','phv_parse','lan_decode_value',
                        'ph_auth_pair','ph_auth_pair_unpair_request','ph_auth_remote_store','m1_automation_execute',
                        'ph_auth_credential_commit']:
            frames,paths=self.fixture('M1');del frames[missing]
            with self.subTest(missing=missing),redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError,'missing actual stack usage'):
                    validate_worker_paths(paths,frames)
        frames,paths=self.fixture('A1');symbols=dict(frames);del frames['install_config']
        with self.assertRaisesRegex(ValueError,'Emitted helper missing'):
            worker_paths(frames,'A1',12288,symbols)
    def test_overflow_dynamic_and_budget_cannot_be_relaxed(self):
        frames,paths=self.fixture('M1');frames['uart_worker']=2200
        with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'> 4096'):
            validate_worker_paths(paths,frames)
        frames,paths=self.fixture('A1');frames.unbounded['feature_worker']='dynamic'
        with self.assertRaisesRegex(ValueError,'unbounded stack usage'):
            validate_worker_paths(paths,frames)
        with self.assertRaisesRegex(ValueError,'budget must remain'):
            worker_paths({},'A1',16384)
        with self.assertRaisesRegex(ValueError,'budget/reserve changed'):
            validate_worker_paths({'relaxed':(12288,1024,['worker'])},{'worker':16})
    def test_retained_part_body_is_added_to_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)/'M1';directory.mkdir()
            (directory/'main.su').write_text('local_auth.c:1:1:ph_auth_pair\t24\tstatic\nlocal_auth.c:2:1:ph_auth_pair.part.1\t832\tstatic\n')
            frames=read_worker_frames(directory)
            self.assertEqual(frames['ph_auth_pair'],856)

if __name__=='__main__':unittest.main()
