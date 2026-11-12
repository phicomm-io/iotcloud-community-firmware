"""Shared diagnostic IDs and read-only dispatch are independent of status cache."""
import importlib.util,json,subprocess,sys,unittest
from pathlib import Path
from tests import test_service_mp as service
ROOT=Path(__file__).resolve().parents[1]
class DiagnosticRegistry(unittest.TestCase):
 def test_generated_ids_match_shared_registry(self):
  subprocess.run([sys.executable,str(ROOT/'tools/generate_diagnostics_schema.py'),'--check'],check=True)
  schema=json.loads((ROOT/'common/diagnostics-schema.json').read_text());fields=schema['fields']
  self.assertEqual(schema['schema'],1);self.assertEqual(schema['encoding'],'id-value-pairs')
  self.assertEqual(len({f['id'] for f in fields}),len(fields));self.assertEqual(len({f['path'] for f in fields}),len(fields))
  # Published initial IDs are immutable, regardless of model/feature macros.
  self.assertEqual({f['path']:f['id'] for f in fields if f['id']<=4},{'last_reset_reason':1,'uptime_ms':2,'uptime_seconds':3,'free_memory':4})
class DiagnosticDispatch(unittest.TestCase):
 run_c=service.NativeService.run_c
 def test_only_explicit_diagnostic_reads_capture_and_invalid_params_never_capture(self):
  self.run_c(r'''
int main(void){
 service_init();ph_buf *out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(captures==1&&!diagnostics);
 for(unsigned i=0;i<3;i++){out=service_query_mp("diagnostics.get");code(out,"ok");ph_buf_free(out);assert(diagnostics==i+1&&captures==1&&!mutations);}
 command_code(request("diagnostics.get",NULL,1),"invalid_params");assert(diagnostics==3&&!mutations);
 out=service_query_mp("status.get");code(out,"ok");ph_buf_free(out);assert(captures==1);
 allocation_budget=0;assert(!service_query_mp("diagnostics.get"));allocation_budget=-1;
 out=service_query_mp("diagnostics.get");code(out,"ok");ph_buf_free(out);assert(!mutations);
 device_config_recovery_set(true);out=service_query_mp("diagnostics.get");code(out,"ok");ph_buf_free(out);
 assert(!mutations);return 0;
}
''')
if __name__=='__main__':unittest.main()
