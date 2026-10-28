"""Run the complete HA writer/parser/discovery matrix with the shared codec ABI."""
from pathlib import Path
import subprocess
import tempfile
import tests.test_ha_json as header_mode
ROOT=Path(__file__).resolve().parents[1]
class SharedHaCodec(header_mode.HaJson):
 def run_c(self,source):
  with tempfile.TemporaryDirectory() as temporary:
   t=Path(temporary);src=t/'test.c';exe=t/'test';src.write_text(source)
   codec=t/'codec.c';codec.write_text('#include "common/ha_codec.inc"\n')
   inputs=[str(src),str(codec)]
   if '#include "common/typed_wire.c"' not in source:
    inputs += [str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c')]
   subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
     '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
     '-DPHICOMM_SHARED_HA_CODEC=1','-I',str(ROOT),*inputs,'-lm','-o',str(exe)],check=True)
   return subprocess.check_output([str(exe)],text=True)
