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
screen = env['PIOENV'] == 'bry88bba15g'
variant = 'hub' if env['PIOENV'] == 'bry88ab-hub' else 'full'

def compress_text(content):
    import zopfli.gzip
    compressed = zopfli.gzip.compress(content, numiterations=500)
    if gzip.decompress(compressed) != content:
        raise RuntimeError('Hub frontend gzip round-trip mismatch')
    return compressed

if local:
    source = Path(local).resolve()
    if variant == 'full':
        spec = importlib.util.spec_from_file_location('development_frontend', source/'common/web/page.py')
        renderer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(renderer)
        data = gzip.compress(renderer.render_page(source/'DAIKIN/web/index.html', minify=True), mtime=0)
    frontend = {'development': True,
                'commit': subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'], text=True).strip(),
                'dirty': bool(subprocess.check_output(['git','-C',str(source),'status','--porcelain'], text=True).strip())}
else:
    bundle = fetch(root/'frontend.lock.json')
    if variant == 'full':
        data = (bundle/'devices/daikin/index.html.gz').read_bytes()
    frontend = json.loads((root/'frontend.lock.json').read_text())
if variant == 'hub':
    hub_root = source/'frontend/dist' if local else bundle/'hub'
    standalone = hub_root/'standalone-daikin.html'
    if not (hub_root/'index.html').is_file() or not standalone.is_file():
        raise RuntimeError('Hub shared frontend missing; build the shared frontend with VITE_HUB=1')
    data = compress_text(standalone.read_bytes())
if local:
    inputs = [p for folder in ('common/web', 'DAIKIN/web') for p in (source/folder).rglob('*') if p.is_file() and p.suffix in ('.js','.css','.html','.py')]
    frontend['source_sha256'] = {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
    patch = subprocess.check_output(['git','-C',str(source),'diff','--binary','HEAD','--','common/web','DAIKIN/web'])
    frontend['tracked_patch_sha256'] = hashlib.sha256(patch).hexdigest()
frontend['gzip_sha256'] = hashlib.sha256(data).hexdigest()
out = root/'DAIKIN/generated'/env['PIOENV']
out.mkdir(parents=True, exist_ok=True)
(out/'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
(out/'frontend.json').write_text(json.dumps(frontend, indent=2)+'\n')
env.Append(CPPPATH=[str(out)])
metadata = resolve_version(root)
if screen:
    # Reuse the pinned full frontend and specialize its product label.
    page = gzip.decompress(data).replace(b'BRY88AX151K', b'BRY88BBA15G').replace(b'BRY88AB151K', b'BRY88BBA15G').replace(b'DAIKIN G1/G2', b'DAIKIN G3')
    # This board has no discrete air-quality lamps or direct CM1106 controls.
    # Keep the shared page/layout, hide only the unsupported product controls.
    board_style = b'<style>section.card:has(#indicator),#indicatorSettings,#climateSettings,#co2Settings{display:none!important}</style>'
    page = page.replace(b'</head>', board_style + b'</head>')
    data = gzip.compress(page, mtime=0)
    (out/'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
    frontend['product'] = 'BRY88BBA15G'
    frontend['adapted_gzip_sha256'] = hashlib.sha256(data).hexdigest()
    (out/'frontend.json').write_text(json.dumps(frontend, indent=2)+'\n')
# Board ABI used by the already installed firmware. The display name is AX.
(out/'build_info.h').write_text('#pragma once\n#define DAIKIN_VERSION ' + json.dumps(metadata['version']) + '\n#define DAIKIN_VARIANT ' + json.dumps(variant) + '\n#define DAIKIN_TARGET ' + json.dumps('BRY88BBA15G' if screen else 'BRY88AB151K') + '\n')
(out/(env['PIOENV']+'-version.json')).write_text(json.dumps(metadata, indent=2)+'\n')
if screen:
    with (out/'build_info.h').open('a') as file:
        file.write('#define BRY_UART_VERSION '+json.dumps('C-'+metadata['git_commit'][:6])+'\n')
env.Append(LINKFLAGS=['-Wl,-u,daikin_target'])

if variant == 'hub':
    # The development override is deliberately visible in package provenance.
    assets = []
    declarations = ['#pragma once', '#include <stddef.h>']
    for path in sorted(p for p in hub_root.rglob('*') if p.is_file()):
        relative = path.relative_to(hub_root).as_posix()
        if relative.endswith('.map') or relative in ('sw.js', 'config.json', 'standalone-daikin.html'):
            continue
        content = path.read_bytes()
        # Images are already compressed; retain their exact bytes in an ordinary
        # gzip wrapper. All textual resources use deterministic host-only Zopfli.
        suffix = path.suffix
        compressed = compress_text(content) if suffix in ('.html','.js','.css','.json','.webmanifest','.svg') else gzip.compress(content, compresslevel=9, mtime=0)
        if gzip.decompress(compressed) != content:
            raise RuntimeError('Hub frontend gzip round-trip mismatch: ' + relative)
        name = 'hub_asset_' + str(len(assets))
        declarations.append('static const unsigned char '+name+'[] = {'+','.join(map(str,compressed))+'};')
        mime = {'.html':'text/html; charset=utf-8','.js':'application/javascript','.css':'text/css','.json':'application/json','.webmanifest':'application/manifest+json','.svg':'image/svg+xml','.png':'image/png','.ico':'image/x-icon'}.get(suffix,'application/octet-stream')
        assets.append('{' + json.dumps('/iotcloud/'+relative) + ',' + json.dumps(mime) + ',' + name + ',sizeof('+name+')}')
    declarations.append('struct HubWebAsset {const char *path,*type;const unsigned char *data;size_t size;};')
    declarations.append('static const HubWebAsset hub_assets[] = {'+','.join(assets)+'};')
    (out/'hub_web.h').write_text('\n'.join(declarations)+'\n')
    frontend['hub_files'] = {str(p.relative_to(hub_root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(hub_root.rglob('*')) if p.is_file()}
    (out/'frontend.json').write_text(json.dumps(frontend,indent=2)+'\n')
