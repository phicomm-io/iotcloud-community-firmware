"""Embed the exact DAIKIN page from the pinned shared frontend release."""
Import('env')
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
root = Path(env['PROJECT_DIR']).parent
sys.path.insert(0, str(root / 'tools'))
from frontend_release import fetch
from check_version import resolve_version
local = os.environ.get('PHICOMM_FRONTEND_SOURCE')
if local:
    source = Path(local).resolve()
    spec = importlib.util.spec_from_file_location('development_frontend', source/'common/web/page.py')
    renderer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(renderer)
    data = gzip.compress(renderer.render_page(source/'DAIKIN/web/index.html', minify=True), mtime=0)
    frontend = {'development': True,
                'commit': subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'], text=True).strip(),
                'dirty': bool(subprocess.check_output(['git','-C',str(source),'status','--porcelain'], text=True).strip())}
else:
    bundle = fetch(root/'frontend.lock.json')
    data = (bundle/'devices/daikin/index.html.gz').read_bytes()
    frontend = json.loads((root/'frontend.lock.json').read_text())
frontend['gzip_sha256'] = hashlib.sha256(data).hexdigest()
out = root/'DAIKIN/generated'
out.mkdir(exist_ok=True)
(out/'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
(out/'frontend.json').write_text(json.dumps(frontend, indent=2)+'\n')
env.Append(CPPPATH=[str(out)])
metadata = resolve_version(root)
# Board ABI used by the already installed firmware. The display name is AX.
(out/'build_info.h').write_text('#pragma once\n#define DAIKIN_VERSION ' + json.dumps(metadata['version']) + '\n#define DAIKIN_TARGET "BRY88AB151K"\n')
(out/(env['PIOENV']+'-version.json')).write_text(json.dumps(metadata, indent=2)+'\n')
env.Append(LINKFLAGS=['-Wl,-u,daikin_target'])
