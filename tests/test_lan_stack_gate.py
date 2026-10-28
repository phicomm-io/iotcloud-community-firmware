"""Missing actual frames, diagnostics, unsafe budget and dynamic frames fail."""
import contextlib
import io
import unittest
import tempfile
from pathlib import Path
from tools.check_lan_stack import lan_paths,validate_lan_paths,add_sha_utils_frames
from tools.check_http_core import StackFrames
class LanStackGate(unittest.TestCase):
 def fixture(self,model):
  paths=lan_paths({},model);frames=StackFrames();frames.update({n:16 for names,r in paths.values() for n in names})
  frames.update({n:16 for n in ('lan_business_input','lan_business_dispatch','lv4_emit','service_command_enter','service_lan_context_valid','runtime_metrics_capture','device_status_visit','emit_string','phv_scan','phv_next','phw_prefix','phw_bytes','bonjour_lan_start','bonjour_lan_field','SHA256Input','SHA224_256ProcessMessageBlock')})
  return frames,lan_paths(frames,model,frames)
 def test_all_models_include_full_business_auth_and_fragment_chain(self):
  for model in ('A1','M1','TC1'):
   frames,paths=self.fixture(model)
   with contextlib.redirect_stdout(io.StringIO()):validate_lan_paths(paths,frames)
   names={n for ns,r in paths.values() for n in ns}
   self.assertTrue({'lan_v4_server_input','service_lan_try_command_mp','mp_dispatch','device_status_mp','service_product_diagnostics_mp','device_diagnostics_mp','service_memory_snapshot',model.lower()+'_diagnostics_capture','ph_auth_pair_resume_boot','phw_bytes_value'}<=names)
 def test_missing_auth_dispatch_status_diagnostics_or_fragment_fails(self):
  for model in ('A1','M1','TC1'):
   for name in ('lan_v4_server_input','service_lan_try_command_mp','mp_dispatch',model.lower()+'_status_capture',model.lower()+'_diagnostics_capture','device_diagnostics_mp','service_memory_snapshot','ph_auth_context_valid','phw_bytes_value','bonjour_lan_txt'):
    frames,paths=self.fixture(model);del frames[name]
    with self.subTest(model=model,name=name),self.assertRaisesRegex(ValueError,'missing actual .su'):
     with contextlib.redirect_stdout(io.StringIO()):validate_lan_paths(paths,frames)
 def test_uninlined_helper_without_su_fails(self):
  with self.assertRaisesRegex(ValueError,'Emitted helper missing'):lan_paths({},'A1',{'lv4_emit':1})
 def test_budget_reserve_and_dynamic_frames_cannot_be_weakened(self):
  frames,paths=self.fixture('A1');frames['a1_diagnostics_capture']=12000
  with contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):validate_lan_paths(paths,frames)
  with self.assertRaisesRegex(ValueError,'budget'):validate_lan_paths(paths,frames,16384)
  with self.assertRaisesRegex(ValueError,'reserve'):validate_lan_paths({'x':(['mp_dispatch'],1)},frames)
  frames,paths=self.fixture('TC1');frames.unbounded['tc1_lan_worker']='dynamic'
  with self.assertRaisesRegex(ValueError,'unbounded'):validate_lan_paths(paths,frames)

 def test_sdk_sha_frames_are_required_when_linked_and_dynamic_fails(self):
  with tempfile.TemporaryDirectory() as directory:
   root=Path(directory);frames=StackFrames();symbols={'SHA256Reset':1,'SHA224_256ProcessMessageBlock':1}
   add_sha_utils_frames(frames,root,{})
   with self.assertRaisesRegex(ValueError,'Missing actual SDK'):
    add_sha_utils_frames(frames,root,symbols)
   path=root/'mico-os/MiCO/security/SHAUtils/sha224-256.su';path.parent.mkdir(parents=True)
   path.write_text('sha.c:10:1:SHA256Reset\t0\tstatic\n')
   with self.assertRaisesRegex(ValueError,'helper missing actual'):
    add_sha_utils_frames(frames,root,symbols)
   path.write_text(path.read_text()+'sha.c:20:1:SHA224_256ProcessMessageBlock\t256\tdynamic\n')
   add_sha_utils_frames(frames,root,symbols)
   self.assertEqual(frames['SHA256Reset'],0)
   self.assertEqual(frames['SHA224_256ProcessMessageBlock'],256)
   with contextlib.redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'unbounded'):
    validate_lan_paths({'sha':(['SHA256Reset','SHA224_256ProcessMessageBlock'],2048)},frames)
