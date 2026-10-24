"""Keep the pinned shared UI; substitute only the device-specific page."""
Import('env')
import sys
from pathlib import Path
import re
import gzip
root = Path(env['PROJECT_DIR']).parent
sys.path.insert(0, str(root / 'tools'))
from frontend_release import fetch
bundle = fetch(root / 'frontend.lock.json')
html = (bundle / 'devices/m1/index.html').read_text()
scripts = list(re.finditer(r'<script>(.*?)</script>', html, flags=re.S))
if not scripts or 'UI.start({model:"M1"' not in scripts[-1].group(1):
    raise RuntimeError('Pinned frontend product entry changed; review adaptation')
page = (root / 'DAIKIN/web/page.js').read_text()
entry = scripts[-1]
html = html[:entry.start()] + '<script>' + page + '</script>' + html[entry.end():]
html = html.replace('<title>M1 空气质量检测器</title>', '<title>DAIKIN 空气检测仪</title>')
data = gzip.compress(html.encode(), mtime=0)
out = root / 'DAIKIN/generated'
out.mkdir(exist_ok=True)
(out / 'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
env.Append(CPPPATH=[str(out)])

import json
from check_version import resolve_version
metadata = resolve_version(root)
target = 'BRY88AB151K'
(out / 'build_info.h').write_text('#pragma once\n#define DAIKIN_VERSION ' + json.dumps(metadata['version']) + '\n#define DAIKIN_TARGET ' + json.dumps(target) + '\n')
(out / (env['PIOENV'] + '-version.json')).write_text(json.dumps(metadata, indent=2) + '\n')
env.Append(LINKFLAGS=['-Wl,-u,daikin_target'])
